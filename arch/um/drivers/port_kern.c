/* 
 * Copyright (C) 2001, 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include "linux/list.h"
#include "linux/sched.h"
#include "linux/slab.h"
#include "linux/interrupt.h"
#include "linux/irq.h"
#include "linux/spinlock.h"
#include "linux/errno.h"
#include "asm/semaphore.h"
#include "asm/errno.h"
#include "kern_util.h"
#include "kern.h"
#include "irq_user.h"
#include "irq_kern.h"
#include "port.h"
#include "init.h"
#include "os.h"

struct port_list {
	struct list_head list;
	int has_connection;
	struct semaphore sem;
	int port;
	int fd;
	spinlock_t lock;
	struct list_head pending;
	struct list_head connections;
};

struct port_dev {
	struct port_list *port;
	int helper_pid;
	int telnetd_pid;
};

struct connection {
	struct list_head list;
	int fd;
	int helper_pid;
	int socket[2];
	int telnetd_pid;
	struct port_list *port;
};

static irqreturn_t pipe_interrupt(int irq, void *data, struct pt_regs *regs)
{
	struct connection *conn = data;
	int fd;

	fd = os_rcv_fd(conn->socket[0], &conn->helper_pid);
	if(fd < 0){
		if(fd == -EAGAIN)
			return(IRQ_NONE);

		printk(KERN_ERR "pipe_interrupt : os_rcv_fd returned %d\n", 
		       -fd);
		os_close_file(conn->fd);
	}

	list_del(&conn->list);

	conn->fd = fd;
	list_add(&conn->list, &conn->port->connections);

	up(&conn->port->sem);
	return(IRQ_HANDLED);
}

static int port_accept(struct port_list *port)
{
	struct connection *conn;
	int fd, socket[2], pid, ret = 0;

	fd = port_connection(port->fd, socket, &pid);
	if(fd < 0){
		if(fd != -EAGAIN)
			printk(KERN_ERR "port_accept : port_connection "
			       "returned %d\n", -fd);
		goto out;
	}

	conn = kmalloc(sizeof(*conn), GFP_ATOMIC);
	if(conn == NULL){
		printk(KERN_ERR "port_accept : failed to allocate "
		       "connection\n");
		goto out_close;
	}
	*conn = ((struct connection) 
		{ .list 	= LIST_HEAD_INIT(conn->list),
		  .fd 		= fd,
		  .socket  	= { socket[0], socket[1] },
		  .telnetd_pid 	= pid,
		  .port 	= port });

	if(um_request_irq(TELNETD_IRQ, socket[0], IRQ_READ, pipe_interrupt, 
			  SA_INTERRUPT | SA_SHIRQ | SA_SAMPLE_RANDOM, 
			  "telnetd", conn)){
		printk(KERN_ERR "port_accept : failed to get IRQ for "
		       "telnetd\n");
		goto out_free;
	}

	list_add(&conn->list, &port->pending);
	return(1);

 out_free:
	kfree(conn);
 out_close:
	os_close_file(fd);
	if(pid != -1) 
		os_kill_process(pid, 1);
 out:
	return(ret);
} 

DECLARE_MUTEX(ports_sem);
struct list_head ports = LIST_HEAD_INIT(ports);

void port_work_proc(void *unused)
{
	struct port_list *port;
	struct list_head *ele;
	unsigned long flags;

	local_irq_save(flags);
	list_for_each(ele, &ports){
		port = list_entry(ele, struct port_list, list);
		if(!port->has_connection)
			continue;
		reactivate_fd(port->fd, ACCEPT_IRQ);
		while(port_accept(port)) ;
		port->has_connection = 0;
	}
	local_irq_restore(flags);
}

DECLARE_WORK(port_work, port_work_proc, NULL);

static irqreturn_t port_interrupt(int irq, void *data, struct pt_regs *regs)
{
	struct port_list *port = data;

	port->has_connection = 1;
	schedule_work(&port_work);
	return(IRQ_HANDLED);
} 

void *port_data(int port_num)
{
	struct list_head *ele;
	struct port_list *port;
	struct port_dev *dev = NULL;
	int fd;

	down(&ports_sem);
	list_for_each(ele, &ports){
		port = list_entry(ele, struct port_list, list);
		if(port->port == port_num) goto found;
	}
	port = kmalloc(sizeof(struct port_list), GFP_KERNEL);
	if(port == NULL){
		printk(KERN_ERR "Allocation of port list failed\n");
		goto out;
	}

	fd = port_listen_fd(port_num);
	if(fd < 0){
		printk(KERN_ERR "binding to port %d failed, errno = %d\n",
		       port_num, -fd);
		goto out_free;
	}
	if(um_request_irq(ACCEPT_IRQ, fd, IRQ_READ, port_interrupt, 
			  SA_INTERRUPT | SA_SHIRQ | SA_SAMPLE_RANDOM, "port",
			  port)){
		printk(KERN_ERR "Failed to get IRQ for port %d\n", port_num);
		goto out_close;
	}

	*port = ((struct port_list) 
		{ .list 	 	= LIST_HEAD_INIT(port->list),
		  .has_connection 	= 0,
		  .sem 			= __SEMAPHORE_INITIALIZER(port->sem, 
								  0),
		  .lock 		= SPIN_LOCK_UNLOCKED,
		  .port 	 	= port_num,
		  .fd  			= fd,
		  .pending 		= LIST_HEAD_INIT(port->pending),
		  .connections 		= LIST_HEAD_INIT(port->connections) });
	list_add(&port->list, &ports);

 found:
	dev = kmalloc(sizeof(struct port_dev), GFP_KERNEL);
	if(dev == NULL){
		printk(KERN_ERR "Allocation of port device entry failed\n");
		goto out;
	}

	*dev = ((struct port_dev) { .port  		= port,
				    .helper_pid  	= -1,
				    .telnetd_pid  	= -1 });
	goto out;

 out_free:
	kfree(port);
 out_close:
	os_close_file(fd);
 out:
	up(&ports_sem);
	return(dev);
}

int port_wait(void *data)
{
	struct port_dev *dev = data;
	struct connection *conn;
	struct port_list *port = dev->port;
	int fd;

	while(1){
		if(down_interruptible(&port->sem))
			return(-ERESTARTSYS);

		spin_lock(&port->lock);

		conn = list_entry(port->connections.next, struct connection, 
				  list);
		list_del(&conn->list);
		spin_unlock(&port->lock);

		os_shutdown_socket(conn->socket[0], 1, 1);
		os_close_file(conn->socket[0]);
		os_shutdown_socket(conn->socket[1], 1, 1);
		os_close_file(conn->socket[1]);	

		/* This is done here because freeing an IRQ can't be done
		 * within the IRQ handler.  So, pipe_interrupt always ups
		 * the semaphore regardless of whether it got a successful
		 * connection.  Then we loop here throwing out failed 
		 * connections until a good one is found.
		 */
		free_irq_by_irq_and_dev(TELNETD_IRQ, conn);
		free_irq(TELNETD_IRQ, conn);

		if(conn->fd >= 0) break;
		os_close_file(conn->fd);
		kfree(conn);
	}

	fd = conn->fd;
	dev->helper_pid = conn->helper_pid;
	dev->telnetd_pid = conn->telnetd_pid;
	kfree(conn);

	return(fd);
}

void port_remove_dev(void *d)
{
	struct port_dev *dev = d;

	if(dev->helper_pid != -1)
		os_kill_process(dev->helper_pid, 0);
	if(dev->telnetd_pid != -1)
		os_kill_process(dev->telnetd_pid, 1);
	dev->helper_pid = -1;
	dev->telnetd_pid = -1;
}

void port_kern_free(void *d)
{
	struct port_dev *dev = d;

	port_remove_dev(dev);
	kfree(dev);
}

static void free_port(void)
{
	struct list_head *ele;
	struct port_list *port;

	list_for_each(ele, &ports){
		port = list_entry(ele, struct port_list, list);
		free_irq_by_fd(port->fd);
		os_close_file(port->fd);
	}
}

__uml_exitcall(free_port);

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
