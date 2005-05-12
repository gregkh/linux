/*
 * hfc_usb.c
 *
 * modular HiSax ISDN driver for Colognechip HFC-USB chip
 *
 * Authors : Peter Sprenger  (sprenger@moving-byters.de)
 *           Martin Bachem   (info@colognechip.com)
 *           based on the first hfc_usb driver of Werner Cornelius (werner@isdn-development.de)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
*/


#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/timer.h>
#include <linux/config.h>
#include <linux/init.h>
#include "hisax.h"
#include <linux/module.h>
#include <linux/kernel_stat.h>
#include <linux/usb.h>
#include <linux/kernel.h>
#include <linux/smp_lock.h>
#include <linux/sched.h>
#include "hisax_if.h"

static const char *hfcusb_revision = "4.0";

/*
	to enable much mire debug messages in this driver, define
			VERBOSE_USB_DEBUG and VERBOSE_ISDN_DEBUG
	below
*/

#define VERBOSE_USB_DEBUG
#define VERBOSE_ISDN_DEBUG

#define INCLUDE_INLINE_FUNCS

#define TRUE  1
#define FALSE 0


/***********/
/* defines */
/***********/
#define HFC_CTRL_TIMEOUT	20  //(HZ * USB_CTRL_GET_TIMEOUT)
/* 5ms timeout writing/reading regs */
#define HFC_TIMER_T3     8000      /* timeout for l1 activation timer */
#define HFC_TIMER_T4     500       /* time for state change interval */

#define HFCUSB_L1_STATECHANGE   0  /* L1 state changed */
#define HFCUSB_L1_DRX           1  /* D-frame received */
#define HFCUSB_L1_ERX           2  /* E-frame received */
#define HFCUSB_L1_DTX           4  /* D-frames completed */

#define MAX_BCH_SIZE        2048   /* allowed B-channel packet size */

#define HFCUSB_RX_THRESHOLD 64     /* threshold for fifo report bit rx */
#define HFCUSB_TX_THRESHOLD 64     /* threshold for fifo report bit tx */

#define HFCUSB_CHIP_ID    0x16     /* Chip ID register index */
#define HFCUSB_CIRM       0x00     /* cirm register index */
#define HFCUSB_USB_SIZE   0x07     /* int length register */
#define HFCUSB_USB_SIZE_I 0x06     /* iso length register */
#define HFCUSB_F_CROSS    0x0b     /* bit order register */
#define HFCUSB_CLKDEL     0x37     /* bit delay register */
#define HFCUSB_CON_HDLC   0xfa     /* channel connect register */
#define HFCUSB_HDLC_PAR   0xfb
#define HFCUSB_SCTRL      0x31     /* S-bus control register (tx) */
#define HFCUSB_SCTRL_E    0x32     /* same for E and special funcs */
#define HFCUSB_SCTRL_R    0x33     /* S-bus control register (rx) */
#define HFCUSB_F_THRES    0x0c     /* threshold register */
#define HFCUSB_FIFO       0x0f     /* fifo select register */
#define HFCUSB_F_USAGE    0x1a     /* fifo usage register */
#define HFCUSB_MST_MODE0  0x14
#define HFCUSB_MST_MODE1  0x15
#define HFCUSB_P_DATA     0x1f
#define HFCUSB_INC_RES_F  0x0e
#define HFCUSB_STATES     0x30

#define HFCUSB_CHIPID 0x40         /* ID value of HFC-USB */

/******************/
/* fifo registers */
/******************/
#define HFCUSB_NUM_FIFOS   8       /* maximum number of fifos */
#define HFCUSB_B1_TX       0       /* index for B1 transmit bulk/int */
#define HFCUSB_B1_RX       1       /* index for B1 receive bulk/int */
#define HFCUSB_B2_TX       2
#define HFCUSB_B2_RX       3
#define HFCUSB_D_TX        4
#define HFCUSB_D_RX        5
#define HFCUSB_PCM_TX      6
#define HFCUSB_PCM_RX      7

/*
* used to switch snd_transfer_mode for different TA modes e.g. the Billion USB TA just
* supports ISO out, while the Cologne Chip EVAL TA just supports BULK out
*/
#define USB_INT		0
#define USB_BULK	1
#define USB_ISOC	2

#define ISOC_PACKETS_D	8
#define ISOC_PACKETS_B	8
#define ISO_BUFFER_SIZE	128

// ISO send definitions
#define SINK_MAX	68
#define SINK_MIN	48
#define SINK_DMIN	12
#define SINK_DMAX	18
#define BITLINE_INF	(-64*8)




/**********/
/* macros */
/**********/
#define write_usb(a,b,c) usb_control_msg((a)->dev,(a)->ctrl_out_pipe,0,0x40,(c),(b),NULL,0,HFC_CTRL_TIMEOUT)
#define read_usb(a,b,c) usb_control_msg((a)->dev,(a)->ctrl_in_pipe,1,0xC0,0,(b),(c),1,HFC_CTRL_TIMEOUT)

/*************************************************/
/* entry and size of output/input control buffer */
/*************************************************/
#define HFC_CTRL_BUFSIZE 32
typedef struct
{
	__u8 hfc_reg;		/* register number */
	__u8 reg_val;		/* value to be written (or read) */
	int action;         /* data for action handler */

} ctrl_buft;

typedef struct
{
	int vendor;         // vendor id
	int prod_id;	    // product id
	char *vend_name;    // vendor string
	__u8 led_scheme;    // led display scheme
	__u8 led_invert;    // invert led aux port settings
	__u8 led_bits[8];   // array of 8 possible LED bitmask settings

} vendor_data;

/***************************************************************/
/* structure defining input+output fifos (interrupt/bulk mode) */
/***************************************************************/

struct usb_fifo;			/* forward definition */
typedef struct iso_urb_struct
{
	struct urb *purb;
	__u8 buffer[ISO_BUFFER_SIZE];	/* buffer incoming/outgoing data */
	struct usb_fifo *owner_fifo;	// pointer to owner fifo
} iso_urb_struct;


struct hfcusb_data;			/* forward definition */
typedef struct usb_fifo
{
	int fifonum;			/* fifo index attached to this structure */
	int active;			/* fifo is currently active */
	struct hfcusb_data *hfc;	/* pointer to main structure */
	int pipe;			/* address of endpoint */
	__u8 usb_packet_maxlen;		/* maximum length for usb transfer */
	unsigned int max_size;		/* maximum size of receive/send packet */
	__u8 intervall;			/* interrupt interval */
	struct sk_buff *skbuff; 	/* actual used buffer */
	struct urb *urb;		/* transfer structure for usb routines */
	__u8 buffer[128];		/* buffer incoming/outgoing data */
	int bit_line;			/* how much bits are in the fifo? */

	volatile __u8 usb_transfer_mode;/* switched between ISO and INT */
	iso_urb_struct iso[2];		/* need two urbs to have one always for pending */
	struct hisax_if *hif;		/* hisax interface */
	int delete_flg;			/* only delete skbuff once */
	int last_urblen;		/* remember length of last packet */

} usb_fifo;


/*********************************************/
/* structure holding all data for one device */
/*********************************************/
typedef struct hfcusb_data
{
	// HiSax Interface for loadable Layer1 drivers
	struct hisax_d_if d_if;			/* see hisax_if.h */
	struct hisax_b_if b_if[2];		/* see hisax_if.h */
	int protocol;
	
	struct usb_device *dev;			/* our device */
	int if_used;				/* used interface number */
	int alt_used;				/* used alternate config */
	int ctrl_paksize;			/* control pipe packet size */
	int ctrl_in_pipe, ctrl_out_pipe;	/* handles for control pipe */
	int cfg_used;				/* configuration index used */
	int vend_idx;				// vendor found

	int b_mode[2];				// B-channel mode

	int l1_activated;			// layer 1 activated

	int packet_size,iso_packet_size;	

	/* control pipe background handling */
	ctrl_buft ctrl_buff[HFC_CTRL_BUFSIZE];	/* buffer holding queued data */
	volatile int ctrl_in_idx, ctrl_out_idx,
		ctrl_cnt;			/* input/output pointer + count */
	struct urb *ctrl_urb;			/* transfer structure for control channel */

	struct usb_ctrlrequest ctrl_write;	/* buffer for control write request */
	struct usb_ctrlrequest ctrl_read;	/* same for read request */

	__u8 led_state,led_new_data,led_b_active;

	volatile __u8 threshold_mask;		/* threshold actually reported */
	volatile __u8 bch_enables;		/* or mask for sctrl_r and sctrl register values */

	usb_fifo fifos[HFCUSB_NUM_FIFOS];	/* structure holding all fifo data */

	volatile __u8 l1_state;			/* actual l1 state */
	struct timer_list t3_timer;		/* timer 3 for activation/deactivation */
	struct timer_list t4_timer;		/* timer 4 for activation/deactivation */
	struct timer_list led_timer;		/* timer flashing leds */

} hfcusb_data;


static void collect_rx_frame(usb_fifo *fifo,__u8 *data,int len,int finish);



/******************************************************/
/* start next background transfer for control channel */
/******************************************************/
static void ctrl_start_transfer(hfcusb_data * hfc)
{
	int err;
	if(hfc->ctrl_cnt)
	{
		hfc->ctrl_urb->pipe = hfc->ctrl_out_pipe;
		hfc->ctrl_urb->setup_packet = (u_char *) & hfc->ctrl_write;
		hfc->ctrl_urb->transfer_buffer = NULL;
		hfc->ctrl_urb->transfer_buffer_length = 0;
		hfc->ctrl_write.wIndex = hfc->ctrl_buff[hfc->ctrl_out_idx].hfc_reg;
		hfc->ctrl_write.wValue = hfc->ctrl_buff[hfc->ctrl_out_idx].reg_val;
		err = usb_submit_urb(hfc->ctrl_urb, GFP_ATOMIC);	/* start transfer */
		printk(KERN_DEBUG "ctrl_start_transfer: submit %d\n", err);
	}
}				/* ctrl_start_transfer */

/************************************/
/* queue a control transfer request */
/* return 0 on success.             */
/************************************/
static int queue_control_request(hfcusb_data * hfc, __u8 reg, __u8 val,int action)
{
	ctrl_buft *buf;

#ifdef VERBOSE_USB_DEBUG
	printk ("HFC_USB: queue_control_request reg: %x, val: %x\n", reg, val);
#endif

	if(hfc->ctrl_cnt >= HFC_CTRL_BUFSIZE)  return(1);	   /* no space left */
	buf = &hfc->ctrl_buff[hfc->ctrl_in_idx];	/* pointer to new index */
	buf->hfc_reg = reg;
	buf->reg_val = val;
	buf->action=action;
	if (++hfc->ctrl_in_idx >= HFC_CTRL_BUFSIZE)
		hfc->ctrl_in_idx = 0;	/* pointer wrap */
	if (++hfc->ctrl_cnt == 1)
		ctrl_start_transfer(hfc);
	return(0);
}		/* queue_control_request */


static int control_action_handler(hfcusb_data *hfc,int reg,int val,int action)
{
	if(!action) return(1);  // no action defined

	return(0);
}


/***************************************************************/
/* control completion routine handling background control cmds */
/***************************************************************/
static void ctrl_complete(struct urb *urb, struct pt_regs *regs)
{
	hfcusb_data *hfc = (hfcusb_data *) urb->context;
	ctrl_buft *buf;

	printk(KERN_DEBUG "ctrl_complete cnt %d\n", hfc->ctrl_cnt);
	urb->dev = hfc->dev;
	if(hfc->ctrl_cnt)
	{
		buf=&hfc->ctrl_buff[hfc->ctrl_out_idx];
		control_action_handler(hfc,buf->hfc_reg,buf->reg_val,buf->action);

		hfc->ctrl_cnt--;	/* decrement actual count */
		if(++hfc->ctrl_out_idx >= HFC_CTRL_BUFSIZE) hfc->ctrl_out_idx = 0;	/* pointer wrap */

		ctrl_start_transfer(hfc);	/* start next transfer */
	}
}				/* ctrl_complete */



#define LED_OFF      0   // no LED support
#define LED_SCHEME1  1	 // LED standard scheme
#define LED_SCHEME2  2	 // not used yet...

#define LED_POWER_ON	1
#define LED_POWER_OFF	2
#define LED_S0_ON		3
#define LED_S0_OFF		4
#define LED_B1_ON		5
#define LED_B1_OFF		6
#define LED_B1_DATA		7
#define LED_B2_ON		8
#define LED_B2_OFF		9
#define LED_B2_DATA	   10

#define LED_NORMAL   0	 // LEDs are normal
#define LED_INVERTED 1   // LEDs are inverted

// time for LED flashing
#define LED_TIME      250

vendor_data vdata[]=
{
    {0x959, 0x2bd0, "ISDN USB TA (Cologne Chip HFC-S USB based)", LED_OFF,LED_NORMAL,{4,0,2,1}},     /* CologneChip Eval TA */
	{0x7b0, 0x0007, "Billion tiny USB ISDN TA 128", LED_SCHEME1,  LED_INVERTED, {8,0x40,0x20,0x10}},  /* Billion TA */
	{0x742, 0x2008, "Stollmann USB TA",             LED_SCHEME1,  LED_NORMAL,   {4,0,2,1}},           /* Stollmann TA */
	{0x8e3, 0x0301, "Olitec USB RNIS",              LED_SCHEME1,  LED_NORMAL,   {2,0,1,4}},           /* Olitec TA  */
	{0x675, 0x1688, "DrayTec USB ISDN TA",          LED_SCHEME1,  LED_NORMAL,   {4,0,2,1}},           /* Draytec TA */
	{0x7fa, 0x0846, "Bewan Modem RNIS USB",         LED_SCHEME1,  LED_INVERTED, {8,0x40,0x20,0x10}},  /* Bewan TA   */
	{0}			   // EOL element
};
										
/***************************************************/
/* write led data to auxport & invert if necessary */
/***************************************************/
static void write_led(hfcusb_data * hfc,__u8 led_state)
{
	if(led_state!=hfc->led_state)
	{
		hfc->led_state=led_state;
		queue_control_request(hfc, HFCUSB_P_DATA,(vdata[hfc->vend_idx].led_invert) ? ~led_state : led_state,1);
	}
}

/******************************************/
/* invert B-channel LEDs if data is sent  */
/******************************************/
static void led_timer(hfcusb_data * hfc)
{
   	static int cnt=0;
	__u8 led_state=hfc->led_state;

	if(cnt)
	{
		if(hfc->led_b_active&1) led_state|=vdata[hfc->vend_idx].led_bits[2];
		if(hfc->led_b_active&2) led_state|=vdata[hfc->vend_idx].led_bits[3];
	}
	else
	{
		if(!(hfc->led_b_active&1) || hfc->led_new_data&1) led_state&=~vdata[hfc->vend_idx].led_bits[2];
		if(!(hfc->led_b_active&2) || hfc->led_new_data&2) led_state&=~vdata[hfc->vend_idx].led_bits[3];
	}

	write_led(hfc,led_state);
	hfc->led_new_data=0;

	cnt=!cnt;
	// restart 4 hz timer
	hfc->led_timer.expires = jiffies + (LED_TIME * HZ) / 1000;
	if(!timer_pending(&hfc->led_timer)) add_timer(&hfc->led_timer);
}

/**************************/
/* handle LED requests    */
/**************************/
static void handle_led(hfcusb_data * hfc,int event)
{
	__u8 led_state=hfc->led_state;

	// if no scheme -> no LED action
   	if(vdata[hfc->vend_idx].led_scheme==LED_OFF) return;

	switch(event)
	{
		case LED_POWER_ON:
				   led_state|=vdata[hfc->vend_idx].led_bits[0];
				break;
		case LED_POWER_OFF: // no Power off handling
				break;
		case LED_S0_ON:
				   led_state|=vdata[hfc->vend_idx].led_bits[1];
				break;
		case LED_S0_OFF:
				   led_state&=~vdata[hfc->vend_idx].led_bits[1];
				break;
		case LED_B1_ON:
					hfc->led_b_active|=1;
				break;
		case LED_B1_OFF:
					hfc->led_b_active&=~1;
				break;
		case LED_B1_DATA:
				   hfc->led_new_data|=1;
				break;
		case LED_B2_ON:
				   hfc->led_b_active|=2;
				break;
		case LED_B2_OFF:
					hfc->led_b_active&=~2;
				break;
		case LED_B2_DATA:
				   hfc->led_new_data|=2;
				break;
	}
	
	write_led(hfc,led_state);
}

/********************************/
/* called when timer t3 expires */
/********************************/
static void l1_timer_expire_t3(hfcusb_data * hfc)
{
    //printk (KERN_INFO "HFC-USB: l1_timer_expire_t3\n");

	hfc->d_if.ifc.l1l2(&hfc->d_if.ifc,PH_DEACTIVATE | INDICATION,NULL);
#ifdef VERBOSE_USB_DEBUG
	printk(KERN_INFO "PH_DEACTIVATE | INDICATION sent\n");
#endif
	hfc->l1_activated=FALSE;
	handle_led(hfc,LED_S0_OFF);
}

/********************************/
/* called when timer t4 expires */
/********************************/
static void l1_timer_expire_t4(hfcusb_data * hfc)
{
    //printk (KERN_INFO "HFC-USB: l1_timer_expire_t4\n");

	hfc->d_if.ifc.l1l2(&hfc->d_if.ifc,PH_DEACTIVATE | INDICATION,NULL);
#ifdef VERBOSE_USB_DEBUG
	printk(KERN_INFO "PH_DEACTIVATE | INDICATION sent\n");
#endif
	hfc->l1_activated=FALSE;
	handle_led(hfc,LED_S0_OFF);
}

/*****************************/
/* handle S0 state changes   */
/*****************************/
static void state_handler(hfcusb_data * hfc,__u8 state)
{
	__u8 old_state;

	old_state=hfc->l1_state;

	// range check
	if(state==old_state || state<1 || state>8) return;

#ifdef VERBOSE_ISDN_DEBUG
	printk(KERN_INFO "HFC-USB: new S0 state:%d old_state:%d\n",state,old_state);
#endif

	if(state<4 || state==7 || state==8)
	{
        if(timer_pending(&hfc->t3_timer)) del_timer(&hfc->t3_timer);
		//printk(KERN_INFO "HFC-USB: T3 deactivated\n");
	}

	if(state>=7)
	{
        if(timer_pending(&hfc->t4_timer)) del_timer(&hfc->t4_timer);
		//printk(KERN_INFO "HFC-USB: T4 deactivated\n");
	}

	if(state==7 && !hfc->l1_activated)
	{
		hfc->d_if.ifc.l1l2(&hfc->d_if.ifc,PH_ACTIVATE | INDICATION,NULL);
		//printk(KERN_INFO "HFC-USB: PH_ACTIVATE | INDICATION sent\n");
		hfc->l1_activated=TRUE;
		handle_led(hfc,LED_S0_ON);
	}
	else
	if(state<=3 /* && activated*/)
	{
		if(old_state==7 || old_state==8)
		{
			//printk(KERN_INFO "HFC-USB: T4 activated\n");
			hfc->t4_timer.expires = jiffies + (HFC_TIMER_T4 * HZ) / 1000;
			if(!timer_pending(&hfc->t4_timer)) add_timer(&hfc->t4_timer);
		}
		else
		{
			hfc->d_if.ifc.l1l2(&hfc->d_if.ifc,PH_DEACTIVATE | INDICATION,NULL);
			//printk(KERN_INFO "HFC-USB: PH_DEACTIVATE | INDICATION sent\n");
			hfc->l1_activated=FALSE;
			handle_led(hfc,LED_S0_OFF);
		}
	}

	hfc->l1_state=state;
}


/* prepare iso urb */
static void fill_isoc_urb(struct urb *urb, struct usb_device *dev, unsigned int pipe, void *buf,
	int num_packets, int packet_size, int interval, usb_complete_t complete, void *context)
{
	int k;

	spin_lock_init(&urb->lock);	// do we really need spin_lock_init ?
	urb->dev = dev;
	urb->pipe = pipe;
	urb->complete = complete;
	urb->number_of_packets = num_packets;
	urb->transfer_buffer_length = packet_size * num_packets;
	urb->context = context;
	urb->transfer_buffer = buf;
	urb->transfer_flags = 0;
	urb->transfer_flags = URB_ISO_ASAP;
	urb->actual_length = 0;
	urb->interval = interval;
	for (k = 0; k < num_packets; k++) {
		urb->iso_frame_desc[k].offset = packet_size * k;
		urb->iso_frame_desc[k].length = packet_size;
		urb->iso_frame_desc[k].actual_length = 0;
	}
}

/* allocs urbs and start isoc transfer with two pending urbs to avoid gaps in the transfer chain */
static int start_isoc_chain(usb_fifo * fifo, int num_packets_per_urb,usb_complete_t complete,int packet_size)
{
	int i, k, errcode;

#ifdef VERBOSE_USB_DEBUG
	printk(KERN_INFO "HFC-USB: starting ISO-chain for Fifo %i\n",  fifo->fifonum);
#endif


	// allocate Memory for Iso out Urbs
	for (i = 0; i < 2; i++) {
		if (!(fifo->iso[i].purb)) {
			fifo->iso[i].purb = usb_alloc_urb(num_packets_per_urb, GFP_KERNEL);
			fifo->iso[i].owner_fifo = (struct usb_fifo *) fifo;

			// Init the first iso
			if (ISO_BUFFER_SIZE >= (fifo->usb_packet_maxlen * num_packets_per_urb))
			{

				fill_isoc_urb(fifo->iso[i].purb, fifo->hfc->dev, fifo->pipe, fifo->iso[i].buffer,
					num_packets_per_urb, fifo->usb_packet_maxlen, fifo->intervall,
					complete, &fifo->iso[i]);

				memset(fifo->iso[i].buffer, 0, sizeof(fifo->iso[i].buffer));

				// defining packet delimeters in fifo->buffer
				for(k = 0; k < num_packets_per_urb; k++)
				{
					fifo->iso[i].purb->iso_frame_desc[k].offset = k*packet_size;
					fifo->iso[i].purb->iso_frame_desc[k].length = packet_size;
				}
			}
		}

		fifo->bit_line = BITLINE_INF;

		errcode = usb_submit_urb(fifo->iso[i].purb, GFP_KERNEL);
		fifo->active = (errcode >= 0) ? 1 : 0;
		if(errcode < 0)
		{
			printk(KERN_INFO "HFC-USB: error submitting ISO URB: %i.%i \n",  errcode, i);
		};

	}

	// errcode = (usb_submit_urb(fifo->iso[0].purb, GFP_KERNEL));
	return(fifo->active);
}

/* stops running iso chain and frees their pending urbs */
static void stop_isoc_chain(usb_fifo * fifo)
{
	int i;

	for(i = 0; i < 2; i++)
	{
		if(fifo->iso[i].purb)
		{
#ifdef VERBOSE_USB_DEBUG
			printk(KERN_INFO "HFC-USB: Stopping iso chain for fifo %i.%i\n", fifo->fifonum, i);
#endif
			usb_unlink_urb(fifo->iso[i].purb);
			usb_free_urb(fifo->iso[i].purb);
			fifo->iso[i].purb = NULL;
		}
	}
	if (fifo->urb) {
		usb_unlink_urb(fifo->urb);
		usb_free_urb(fifo->urb);
		fifo->urb = NULL;
	}
	fifo->active = 0;
}

// defines how much ISO packets are handled in one URB
static int iso_packets[8]={ISOC_PACKETS_B,ISOC_PACKETS_B,ISOC_PACKETS_B,ISOC_PACKETS_B,
	                       ISOC_PACKETS_D,ISOC_PACKETS_D,ISOC_PACKETS_D,ISOC_PACKETS_D};

/*****************************************************/
/* transmit completion routine for all ISO tx fifos */
/*****************************************************/
static void tx_iso_complete(struct urb *urb, struct pt_regs *regs)
{
	iso_urb_struct *context_iso_urb = (iso_urb_struct *) urb->context;
	usb_fifo *fifo = context_iso_urb->owner_fifo;
	hfcusb_data *hfc = fifo->hfc;
	int k, tx_offset, num_isoc_packets, sink, len, current_len,errcode,frame_complete,transp_mode,fifon;
	__u8 threshbit;
	__u8 threshtable[8] = { 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80};

	fifon=fifo->fifonum;
	tx_offset=0;
	// very weird error code when using ohci drivers, for now : ignore this error ...  (MB)
	if(urb->status == -EOVERFLOW)
	{
		urb->status = 0;
#ifdef VERBOSE_USB_DEBUG
		printk(KERN_INFO "HFC-USB: ignoring USB DATAOVERRUN  for fifo  %i \n",fifon);
#endif
	}

	if(fifo->active && !urb->status)
	{
		transp_mode=0;
		if(fifon<4 && hfc->b_mode[fifon/2]==L1_MODE_TRANS) transp_mode=TRUE;

		threshbit = threshtable[fifon] & hfc->threshold_mask;	// is threshold set for our channel?
		num_isoc_packets=iso_packets[fifon];

		if(fifon >= HFCUSB_D_TX)
		{
			sink = (threshbit) ? SINK_DMIN : SINK_DMAX;	// how much bit go to the sink for D-channel?
		}
		else
		{
			sink = (threshbit) ? SINK_MIN : SINK_MAX;	// how much bit go to the sink for B-channel?
		}

		// prepare ISO Urb
		fill_isoc_urb(urb, fifo->hfc->dev, fifo->pipe,context_iso_urb->buffer, num_isoc_packets,
			fifo->usb_packet_maxlen, fifo->intervall, tx_iso_complete, urb->context);
		memset(context_iso_urb->buffer, 0, sizeof(context_iso_urb->buffer));

		frame_complete=FALSE;

		// Generate Iso Packets
		for(k = 0; k < num_isoc_packets; ++k)
		{
			if(fifo->skbuff)
			{
				len = fifo->skbuff->len;	// remaining length

				fifo->bit_line -= sink;	// we lower data margin every msec
				current_len = (0 - fifo->bit_line) / 8;
				if(current_len > 14) current_len = 14;	// maximum 15 byte for every ISO packet makes our life easier
				current_len = (len <= current_len) ? len : current_len;
				fifo->bit_line += current_len * 8;	// how much bit do we put on the line?

				context_iso_urb->buffer[tx_offset] = 0;
				if(current_len == len)
				{
					if(!transp_mode)
					{
						context_iso_urb->buffer[tx_offset] = 1;	// here frame completion
						fifo->bit_line += 32;	// add 2 byte flags and 16bit CRC at end of ISDN frame
					}
					frame_complete = TRUE;
				}

				// copy bytes from buffer into ISO_URB
				memcpy(context_iso_urb->buffer+tx_offset+1,fifo->skbuff->data,current_len);
				skb_pull(fifo->skbuff,current_len);

				// define packet delimeters within the URB buffer
				urb->iso_frame_desc[k].offset = tx_offset;
				urb->iso_frame_desc[k].length = current_len + 1;

				tx_offset += (current_len + 1);
				// printk(KERN_INFO "HFC-USB: fifonum:%d,%d bytes to send, %d bytes ISO packet,bitline:%d,sink:%d,threshbit:%d,threshmask:%x\n",fifon,len,current_len,fifo->bit_line,sink,threshbit,hfc->threshold_mask);
				if(!transp_mode)
				{
					if(fifon==HFCUSB_B1_TX) handle_led(hfc,LED_B1_DATA);
					if(fifon==HFCUSB_B2_TX) handle_led(hfc,LED_B2_DATA);
				}
			}
			else
			{
				// we have no more data - generate 1 byte ISO packets
				urb->iso_frame_desc[k].offset = tx_offset++;

				urb->iso_frame_desc[k].length = 1;
				fifo->bit_line -= sink;	// we lower data margin every msec

				if(fifo->bit_line < BITLINE_INF)
				{
					fifo->bit_line = BITLINE_INF;
					//printk (KERN_INFO "HFC-USB: BITLINE_INF underrun\n");
				}
			}

			if(frame_complete)
			{
				// delete the buffer only once, here or in hfc_usb_l2l1() in a PH_DATA|REQUEST
				fifo->delete_flg=TRUE;

				fifo->hif->l1l2(fifo->hif,PH_DATA|CONFIRM,(void*)fifo->skbuff->truesize);

				if(fifo->skbuff && fifo->delete_flg)
				{
					dev_kfree_skb_any(fifo->skbuff);
					//printk(KERN_INFO "HFC-USB: skbuff=NULL on fifo:%d\n",fifo->fifonum);
					fifo->skbuff = NULL;
					fifo->delete_flg=FALSE;
				}

				frame_complete=FALSE;
			}
        }

		errcode = usb_submit_urb(urb, GFP_ATOMIC);
		if(errcode < 0)
		{
			printk(KERN_INFO "HFC-USB: error submitting ISO URB: %i \n",  errcode);
		}
	}
	else
	{
		if(urb->status)
		{
			printk(KERN_INFO "HFC-USB: tx_iso_complete : urb->status %i, fifonum %i\n",  urb->status,fifon);
		}
	}

}				/* tx_iso_complete */

/*****************************************************/
/* receive completion routine for all ISO tx fifos   */
/*****************************************************/
static void rx_iso_complete(struct urb *urb, struct pt_regs *regs)
{
	iso_urb_struct *context_iso_urb = (iso_urb_struct *) urb->context;
	usb_fifo *fifo = context_iso_urb->owner_fifo;
	hfcusb_data *hfc = fifo->hfc;
	int k, len, errcode, offset, num_isoc_packets,fifon;
	__u8 *buf;

	fifon=fifo->fifonum;
	// very weird error code when using ohci drivers, for now : ignore this error ...  (MB)
	if(urb->status == -EOVERFLOW)
	{
		urb->status = 0;
#ifdef VERBOSE_USB_DEBUG
		printk(KERN_INFO "HFC-USB: ignoring USB DATAOVERRUN  for fifo  %i \n",fifon);
#endif
	}

	if(fifo->active && !urb->status)
	{
		num_isoc_packets=iso_packets[fifon];

		// Generate D-Channel Iso Packets
		for(k = 0; k < num_isoc_packets; ++k)
		{
			len=urb->iso_frame_desc[k].actual_length;
			offset=urb->iso_frame_desc[k].offset;
			buf=context_iso_urb->buffer+offset;

			if(fifo->last_urblen!=fifo->usb_packet_maxlen)
			{
				// the threshold mask is in the 2nd status byte
				hfc->threshold_mask=buf[1];
				// the S0 state is in the upper half of the 1st status byte
				state_handler(hfc,buf[0] >> 4);
				// if we have more than the 2 status bytes -> collect data
				if(len>2) collect_rx_frame(fifo,buf+2,len-2,buf[0]&1);
			}
			else collect_rx_frame(fifo,buf,len,0);

			fifo->last_urblen=len;

        }

		// prepare ISO Urb
		fill_isoc_urb(urb, fifo->hfc->dev, fifo->pipe,context_iso_urb->buffer, num_isoc_packets,
			fifo->usb_packet_maxlen, fifo->intervall, rx_iso_complete, urb->context);

		errcode = usb_submit_urb(urb, GFP_ATOMIC);
		if(errcode < 0)
		{
			printk(KERN_INFO "HFC-USB: error submitting ISO URB: %i \n",  errcode);
		}
	}
	else
	{
		if(urb->status)
		{
			printk(KERN_INFO "HFC-USB: rx_iso_complete : urb->status %i, fifonum %i\n",  urb->status,fifon);
		}
	}
}				/* rx_iso_complete */


/*****************************************************/
/* collect data from interrupt or isochron in        */
/*****************************************************/
static void collect_rx_frame(usb_fifo *fifo,__u8 *data,int len,int finish)
{
	hfcusb_data *hfc = fifo->hfc;
	int transp_mode,fifon;

	fifon=fifo->fifonum;
	transp_mode=0;
	if(fifon<4 && hfc->b_mode[fifon/2]==L1_MODE_TRANS) transp_mode=TRUE;

	//printk(KERN_INFO "HFC-USB: got %d bytes finish:%d max_size:%d fifo:%d\n",len,finish,fifo->max_size,fifon);
	if(!fifo->skbuff)
	{
		// allocate sk buffer
		fifo->skbuff=dev_alloc_skb(fifo->max_size + 3);
		if(!fifo->skbuff)
		{
			printk(KERN_INFO "HFC-USB: cannot allocate buffer (dev_alloc_skb) fifo:%d\n",fifon);
			return;
		}
		
	}

	if(len && fifo->skbuff->len+len<fifo->max_size)
	{
		memcpy(skb_put(fifo->skbuff,len),data,len);
	}
	else printk(KERN_INFO "HCF-USB: got frame exceeded fifo->max_size:%d\n",fifo->max_size);

	// give transparent data up, when 128 byte are available
	if(transp_mode && fifo->skbuff->len>=128)
	{
		fifo->hif->l1l2(fifo->hif,PH_DATA | INDICATION,fifo->skbuff);
		fifo->skbuff = NULL;  // buffer was freed from upper layer
		return;
	}

	// we have a complete hdlc packet
	if(finish)
	{
		if(!fifo->skbuff->data[fifo->skbuff->len-1])
		{
			skb_trim(fifo->skbuff,fifo->skbuff->len-3);  // remove CRC & status

			//printk(KERN_INFO "HFC-USB: got frame %d bytes on fifo:%d\n",fifo->skbuff->len,fifon);

			if(fifon==HFCUSB_PCM_RX) fifo->hif->l1l2(fifo->hif,PH_DATA_E | INDICATION,fifo->skbuff);
			else fifo->hif->l1l2(fifo->hif,PH_DATA | INDICATION,fifo->skbuff);

			fifo->skbuff = NULL;  // buffer was freed from upper layer
		}
		else
		{
			printk(KERN_INFO "HFC-USB: got frame %d bytes but CRC ERROR!!!\n",fifo->skbuff->len);

			skb_trim(fifo->skbuff,0);  // clear whole buffer
		}
	}

	// LED flashing only in HDLC mode
	if(!transp_mode)
	{
		if(fifon==HFCUSB_B1_RX) handle_led(hfc,LED_B1_DATA);
		if(fifon==HFCUSB_B2_RX) handle_led(hfc,LED_B2_DATA);
	}
}

/***********************************************/
/* receive completion routine for all rx fifos */
/***********************************************/
static void rx_complete(struct urb *urb, struct pt_regs *regs)
{
	int len;
	__u8 *buf;
	usb_fifo *fifo = (usb_fifo *) urb->context;	/* pointer to our fifo */
	hfcusb_data *hfc = fifo->hfc;

	urb->dev = hfc->dev;	/* security init */

	if((!fifo->active) || (urb->status)) {
#ifdef VERBOSE_USB_DEBUG
		printk(KERN_INFO "HFC-USB: RX-Fifo %i is going down (%i)\n", fifo->fifonum, urb->status);
#endif
		fifo->urb->interval = 0;	/* cancel automatic rescheduling */
		if(fifo->skbuff) {
			dev_kfree_skb_any(fifo->skbuff);
			fifo->skbuff = NULL;
		}
		return;
	}

	len=urb->actual_length;
	buf=fifo->buffer;

	if(fifo->last_urblen!=fifo->usb_packet_maxlen) {
		// the threshold mask is in the 2nd status byte
		hfc->threshold_mask=buf[1];
		// the S0 state is in the upper half of the 1st status byte
		state_handler(hfc,buf[0] >> 4);
		// if we have more than the 2 status bytes -> collect data
		if(len>2) collect_rx_frame(fifo,buf+2,urb->actual_length-2,buf[0]&1);
	} else
		collect_rx_frame(fifo,buf,urb->actual_length,0);

	fifo->last_urblen=urb->actual_length;


}	/* rx_complete */



/***************************************************/
/* start the interrupt transfer for the given fifo */
/***************************************************/
static void start_int_fifo(usb_fifo * fifo)
{
	int errcode;

#ifdef VERBOSE_USB_DEBUG
	printk(KERN_INFO "HFC-USB: starting intr IN fifo:%d\n", fifo->fifonum);
#endif
	if (!fifo->urb) {
		fifo->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!fifo->urb)
			return;
	}
	usb_fill_int_urb(fifo->urb, fifo->hfc->dev, fifo->pipe, fifo->buffer,
				 fifo->usb_packet_maxlen, rx_complete, fifo, fifo->intervall);
	fifo->active = 1;		/* must be marked active */
	errcode = usb_submit_urb(fifo->urb, GFP_KERNEL);

	if(errcode)
	{
		printk(KERN_INFO "HFC-USB: submit URB error(start_int_info): status:%i\n",   errcode);
		fifo->active = 0;
		fifo->skbuff = NULL;
	}
} /* start_int_fifo */

/*****************************/
/* set the B-channel mode    */
/*****************************/
static void set_hfcmode(hfcusb_data *hfc,int channel,int mode)
{
	__u8 val,idx_table[2]={0,2};

#ifdef VERBOSE_ISDN_DEBUG
  printk (KERN_INFO "HFC-USB: setting channel %d to mode %d\n",channel,mode);
#endif

	hfc->b_mode[channel]=mode;

	// setup CON_HDLC
	val=0;
	if(mode!=L1_MODE_NULL) val=8;    // enable fifo?
	if(mode==L1_MODE_TRANS) val|=2;  // set transparent bit

	queue_control_request(hfc,HFCUSB_FIFO,idx_table[channel],1); // set FIFO to transmit register
	queue_control_request(hfc,HFCUSB_CON_HDLC,val,1);
	queue_control_request(hfc,HFCUSB_INC_RES_F,2,1); // reset fifo

	queue_control_request(hfc,HFCUSB_FIFO,idx_table[channel]+1,1); // set FIFO to receive register
	queue_control_request(hfc,HFCUSB_CON_HDLC,val,1);
	queue_control_request(hfc,HFCUSB_INC_RES_F,2,1);  // reset fifo

	val=0x40;
	if(hfc->b_mode[0]) val|=1;
	if(hfc->b_mode[1]) val|=2;
	queue_control_request(hfc,HFCUSB_SCTRL,val,1);

	val=0;
	if(hfc->b_mode[0]) val|=1;
	if(hfc->b_mode[1]) val|=2;
	queue_control_request(hfc,HFCUSB_SCTRL_R,val,1);

	if(mode==L1_MODE_NULL)
	{
		if(channel) handle_led(hfc,LED_B2_OFF);
		else handle_led(hfc,LED_B1_OFF);
	}
	else
	{
		if(channel) handle_led(hfc,LED_B2_ON);
		else handle_led(hfc,LED_B1_ON);
	}
}

/*
   --------------------------------------------------------------------------------------
   from here : hisax_if callback routines :
     - void hfc_usb_d_l2l1(struct hisax_if *hisax_d_if, int pr, void *arg) {

   l1 to l2 routines :
     - static void hfc_usb_l1l2(hfcusb_data * hfc)

*/

void hfc_usb_l2l1(struct hisax_if *my_hisax_if, int pr, void *arg)
{
    usb_fifo *fifo = my_hisax_if->priv;
	hfcusb_data *hfc = fifo->hfc;

    switch (pr) {
		case PH_ACTIVATE | REQUEST:
				if(fifo->fifonum==HFCUSB_D_TX)
				{
#ifdef VERBOSE_ISDN_DEBUG
					printk (KERN_INFO "HFC_USB: hfc_usb_d_l2l1 D-chan: PH_ACTIVATE | REQUEST\n");
#endif
					queue_control_request(hfc, HFCUSB_STATES,0x60,1);	/* make activation */
					hfc->t3_timer.expires = jiffies + (HFC_TIMER_T3 * HZ) / 1000;
					if(!timer_pending(&hfc->t3_timer)) add_timer(&hfc->t3_timer);
				}
				else
				{
#ifdef VERBOSE_ISDN_DEBUG
					printk (KERN_INFO "HFC_USB: hfc_usb_d_l2l1 Bx-chan: PH_ACTIVATE | REQUEST\n");
#endif
					set_hfcmode(hfc,(fifo->fifonum==HFCUSB_B1_TX) ? 0 : 1 ,(int)arg);
					fifo->hif->l1l2(fifo->hif,PH_ACTIVATE | INDICATION, NULL);
				}
                break;
        case PH_DEACTIVATE | REQUEST:
				if(fifo->fifonum==HFCUSB_D_TX)
				{
#ifdef VERBOSE_ISDN_DEBUG
					printk (KERN_INFO "HFC_USB: hfc_usb_d_l2l1 D-chan: PH_DEACTIVATE | REQUEST\n");
#endif
					printk (KERN_INFO "HFC-USB: ISDN TE device should not deativate...\n");
				}
				else
				{
#ifdef VERBOSE_ISDN_DEBUG
					printk (KERN_INFO "HFC_USB: hfc_usb_d_l2l1 Bx-chan: PH_DEACTIVATE | REQUEST\n");
#endif
					set_hfcmode(hfc,(fifo->fifonum==HFCUSB_B1_TX) ? 0 : 1 ,(int)L1_MODE_NULL);
					fifo->hif->l1l2(fifo->hif,PH_DEACTIVATE | INDICATION, NULL);
				}
                break;
		case PH_DATA | REQUEST:
				if(fifo->skbuff && fifo->delete_flg)
				{
					dev_kfree_skb_any(fifo->skbuff);
					//printk(KERN_INFO "skbuff=NULL on fifo:%d\n",fifo->fifonum);
					fifo->skbuff = NULL;
					fifo->delete_flg=FALSE;
				}

				fifo->skbuff=arg; // we have a new buffer

				//if(fifo->fifonum==HFCUSB_D_TX) printk (KERN_INFO "HFC_USB: hfc_usb_d_l2l1 D-chan: PH_DATA | REQUEST\n");
				//else printk (KERN_INFO "HFC_USB: hfc_usb_d_l2l1 Bx-chan: PH_DATA | REQUEST\n");
                break;
        default:
                printk (KERN_INFO "HFC_USB: hfc_usb_d_l2l1: unkown state : %#x\n", pr);
                break;
    }
}

// valid configurations
#define CNF_4INT3ISO  1      // 4 INT IN, 3 ISO OUT
#define CNF_3INT3ISO  2      // 3 INT IN, 3 ISO OUT
#define CNF_4ISO3ISO  3      // 4 ISO IN, 3 ISO OUT
#define CNF_3ISO3ISO  4 	 // 3 ISO IN, 3 ISO OUT


/*
   --------------------------------------------------------------------------------------
   From here on USB initialization and deactivation related routines are implemented :

   - hfc_usb_init :
      is the main Entry Point for the USB Subsystem when the device get plugged
      in. This function calls usb_register with usb_driver as parameter.
      Here, further entry points for probing (hfc_usb_probe) and disconnecting
      the device (hfc_usb_disconnect) are published, as the id_table

   - hfc_usb_probe
      this function is called by the usb subsystem, and steps through the alternate
      settings of the currently plugged in device to detect all Endpoints needed to
      run an ISDN TA.
      Needed EndPoints are
      3 (+1) IntIn EndPoints   (D-in,  E-in, B1-in, B2-in, (E-in)) or
      3 (+1) Isochron In Endpoints (D-out, B1-out, B2-out) and 3 IsoOut Endpoints
      The currently used transfer mode of on the Out-Endpoints will be stored in
      hfc->usb_transfer_mode and is either USB_INT or USB_ISO
      When a valid alternate setting could be found, the usb_init (see blow)
      function is called

   - usb_init
      Here, the HFC_USB Chip itself gets initialized and the USB framework to send/receive
      Data to/from the several EndPoints are initialized:
       The E- and D-Channel Int-In chain gets started
       The IsoChain for the Iso-Out traffic get started

   - hfc_usb_disconnect
      this function is called by the usb subsystem and has to free all resources
      and stop all usb traffic to allow a proper hotplugging disconnect.

*/

/***************************************************************************/
/* usb_init is called once when a new matching device is detected to setup */
/* main parameters. It registers the driver at the main hisax module.       */
/* on success 0 is returned.                                               */
/***************************************************************************/
static int usb_init(hfcusb_data * hfc)
{
	usb_fifo *fifo;
	int i, err;
	u_char b;
	struct hisax_b_if *p_b_if[2];
	
	/* check the chip id */
	printk(KERN_INFO "HFCUSB_CHIP_ID begin\n");
	if (read_usb(hfc, HFCUSB_CHIP_ID, &b) != 1) {
		printk(KERN_INFO "HFC-USB: cannot read chip id\n");
		return(1); 
	}
	printk(KERN_INFO "HFCUSB_CHIP_ID %x\n", b);
	if (b != HFCUSB_CHIPID) {
		printk(KERN_INFO "HFC-USB: Invalid chip id 0x%02x\n", b);
		return(1);
	}

	/* first set the needed config, interface and alternate */
	printk(KERN_INFO "usb_init 1\n");
//	usb_set_configuration(hfc->dev, 1);
	printk(KERN_INFO "usb_init 2\n");
	err = usb_set_interface(hfc->dev, hfc->if_used, hfc->alt_used);
	printk(KERN_INFO "usb_init usb_set_interface return %d\n", err);
	/* now we initialize the chip */
	write_usb(hfc, HFCUSB_CIRM, 8);	    // do reset
	write_usb(hfc, HFCUSB_CIRM, 0x10);	// aux = output, reset off

	// set USB_SIZE to match the the wMaxPacketSize for INT or BULK transfers
	write_usb(hfc, HFCUSB_USB_SIZE,(hfc->packet_size/8) | ((hfc->packet_size/8) << 4));

	// set USB_SIZE_I to match the the wMaxPacketSize for ISO transfers
	write_usb(hfc, HFCUSB_USB_SIZE_I, hfc->iso_packet_size);

	/* enable PCM/GCI master mode */
	write_usb(hfc, HFCUSB_MST_MODE1, 0);	/* set default values */
	write_usb(hfc, HFCUSB_MST_MODE0, 1);	/* enable master mode */

	/* init the fifos */
	write_usb(hfc, HFCUSB_F_THRES, (HFCUSB_TX_THRESHOLD/8) |((HFCUSB_RX_THRESHOLD/8) << 4));

	fifo = hfc->fifos;
	for(i = 0; i < HFCUSB_NUM_FIFOS; i++)
	{
		write_usb(hfc, HFCUSB_FIFO, i);	/* select the desired fifo */
		fifo[i].skbuff = NULL;	/* init buffer pointer */
		fifo[i].max_size = (i <= HFCUSB_B2_RX) ? MAX_BCH_SIZE : MAX_DFRAME_LEN;
		fifo[i].last_urblen=0;
		write_usb(hfc, HFCUSB_HDLC_PAR, ((i <= HFCUSB_B2_RX) ? 0 : 2));	    // set 2 bit for D- & E-channel
		write_usb(hfc, HFCUSB_CON_HDLC, ((i==HFCUSB_D_TX) ? 0x09 : 0x08));	// rx hdlc, enable IFF for D-channel
		write_usb(hfc, HFCUSB_INC_RES_F, 2);	/* reset the fifo */
	}

	write_usb(hfc, HFCUSB_CLKDEL, 0x0f);	 /* clock delay value */
	write_usb(hfc, HFCUSB_STATES, 3 | 0x10); /* set deactivated mode */
	write_usb(hfc, HFCUSB_STATES, 3);	     /* enable state machine */

	write_usb(hfc, HFCUSB_SCTRL_R, 0);	     /* disable both B receivers */
	write_usb(hfc, HFCUSB_SCTRL, 0x40);	     /* disable B transmitters + capacitive mode */

	// set both B-channel to not connected
	hfc->b_mode[0]=L1_MODE_NULL;
	hfc->b_mode[1]=L1_MODE_NULL;

	hfc->l1_activated=FALSE;
	hfc->led_state=0;
	hfc->led_new_data=0;

	/* init the t3 timer */
	init_timer(&hfc->t3_timer);
	hfc->t3_timer.data = (long) hfc;
	hfc->t3_timer.function = (void *) l1_timer_expire_t3;
	/* init the t4 timer */
	init_timer(&hfc->t4_timer);
	hfc->t4_timer.data = (long) hfc;
	hfc->t4_timer.function = (void *) l1_timer_expire_t4;
	/* init the led timer */
	init_timer(&hfc->led_timer);
	hfc->led_timer.data = (long) hfc;
	hfc->led_timer.function = (void *) led_timer;
	// trigger 4 hz led timer
	hfc->led_timer.expires = jiffies + (LED_TIME * HZ) / 1000;
	if(!timer_pending(&hfc->led_timer)) add_timer(&hfc->led_timer);

	// init the background machinery for control requests
	hfc->ctrl_read.bRequestType = 0xc0;
	hfc->ctrl_read.bRequest = 1;
	hfc->ctrl_read.wLength = 1;
	hfc->ctrl_write.bRequestType = 0x40;
	hfc->ctrl_write.bRequest = 0;
	hfc->ctrl_write.wLength = 0;
	usb_fill_control_urb(hfc->ctrl_urb, hfc->dev, hfc->ctrl_out_pipe,(u_char *) & hfc->ctrl_write, NULL, 0, ctrl_complete, hfc);
					
	/* Init All Fifos */
	for(i = 0; i < HFCUSB_NUM_FIFOS; i++)
	{
		hfc->fifos[i].iso[0].purb = NULL;
		hfc->fifos[i].iso[1].purb = NULL;
		hfc->fifos[i].active = 0;
	}

	// register like Germaschewski :
	hfc->d_if.owner = THIS_MODULE;
	hfc->d_if.ifc.priv = &hfc->fifos[HFCUSB_D_TX];
	hfc->d_if.ifc.l2l1 = hfc_usb_l2l1;

	for (i=0; i<2; i++)
	{
		hfc->b_if[i].ifc.priv = &hfc->fifos[HFCUSB_B1_TX+i*2];
		hfc->b_if[i].ifc.l2l1 = hfc_usb_l2l1;
		p_b_if[i] = &hfc->b_if[i];
	}
	
	hfc->protocol = 2;  /* default EURO ISDN, should be a module_param */
	hisax_register(&hfc->d_if, p_b_if, "hfc_usb", hfc->protocol);
	
	for (i=0; i<4; i++)
		hfc->fifos[i].hif=&p_b_if[i/2]->ifc;
	for (i=4; i<8; i++)
		hfc->fifos[i].hif=&hfc->d_if.ifc;

	// 3 (+1) INT IN + 3 ISO OUT
	if(hfc->cfg_used == CNF_3INT3ISO || hfc->cfg_used == CNF_4INT3ISO)
	{
		start_int_fifo(hfc->fifos + HFCUSB_D_RX);	// Int IN D-fifo
		if(hfc->fifos[HFCUSB_PCM_RX].pipe) start_int_fifo(hfc->fifos + HFCUSB_PCM_RX);	// E-fifo
		start_int_fifo(hfc->fifos + HFCUSB_B1_RX);	// Int IN B1-fifo
		start_int_fifo(hfc->fifos + HFCUSB_B2_RX);	// Int IN B2-fifo
	}

	// 3 (+1) ISO IN + 3 ISO OUT
	if(hfc->cfg_used==CNF_3ISO3ISO || hfc->cfg_used==CNF_4ISO3ISO)
	{
		start_isoc_chain(hfc->fifos + HFCUSB_D_RX, ISOC_PACKETS_D, rx_iso_complete,16);
		if(hfc->fifos[HFCUSB_PCM_RX].pipe) start_isoc_chain(hfc->fifos + HFCUSB_PCM_RX, ISOC_PACKETS_D, rx_iso_complete,16);
		start_isoc_chain(hfc->fifos + HFCUSB_B1_RX, ISOC_PACKETS_B, rx_iso_complete,16);
		start_isoc_chain(hfc->fifos + HFCUSB_B2_RX, ISOC_PACKETS_B, rx_iso_complete,16);
	}

	start_isoc_chain(hfc->fifos + HFCUSB_D_TX, ISOC_PACKETS_D, tx_iso_complete,1);
	start_isoc_chain(hfc->fifos + HFCUSB_B1_TX, ISOC_PACKETS_B, tx_iso_complete,1);
	start_isoc_chain(hfc->fifos + HFCUSB_B2_TX, ISOC_PACKETS_B, tx_iso_complete,1);

	handle_led(hfc,LED_POWER_ON);

	return(0);
}	/* usb_init */


/****************************************/
/* data defining the devices to be used */
/****************************************/
// static __devinitdata const struct usb_device_id hfc_usb_idtab[3] = {
static struct usb_device_id hfc_usb_idtab[] = {
	{USB_DEVICE(0x7b0, 0x0007)},	/* Billion USB TA 2 */
	{USB_DEVICE(0x742, 0x2008)},	/* Stollmann USB TA */
	{USB_DEVICE(0x959, 0x2bd0)},	/* Colognechip USB eval TA */
	{USB_DEVICE(0x8e3, 0x0301)},	/* OliTec ISDN USB */
	{USB_DEVICE(0x675, 0x1688)},	/* DrayTec ISDN USB */
	{USB_DEVICE(0x7fa, 0x0846)},    /* Bewan ISDN USB TA */
	{}				/* end with an all-zeroes entry */
};

MODULE_AUTHOR("Peter Sprenger (sprenger@moving-byters.de)/Martin Bachem (info@colognechip.com)");
MODULE_DESCRIPTION("HFC I4L USB driver");
MODULE_DEVICE_TABLE(usb, hfc_usb_idtab);
MODULE_LICENSE("GPL");

#define EP_NUL 1    // Endpoint at this position not allowed
#define EP_NOP 2	// all type of endpoints allowed at this position
#define EP_ISO 3	// Isochron endpoint mandatory at this position
#define EP_BLK 4	// Bulk endpoint mandatory at this position
#define EP_INT 5	// Interrupt endpoint mandatory at this position

// this array represents all endpoints possible in the HCF-USB
// the last 2 entries are the configuration number and the minimum interval for Interrupt endpoints
int validconf[][18]=
{
	// INT in, ISO out config
	{EP_NUL,EP_INT,EP_NUL,EP_INT,EP_NUL,EP_INT,EP_NOP,EP_INT,EP_ISO,EP_NUL,EP_ISO,EP_NUL,EP_ISO,EP_NUL,EP_NUL,EP_NUL,CNF_4INT3ISO,2},
	{EP_NUL,EP_INT,EP_NUL,EP_INT,EP_NUL,EP_INT,EP_NUL,EP_NUL,EP_ISO,EP_NUL,EP_ISO,EP_NUL,EP_ISO,EP_NUL,EP_NUL,EP_NUL,CNF_3INT3ISO,2},
	// ISO in, ISO out config
	{EP_NUL,EP_NUL,EP_NUL,EP_NUL,EP_NUL,EP_NUL,EP_NUL,EP_NUL,EP_ISO,EP_ISO,EP_ISO,EP_ISO,EP_ISO,EP_ISO,EP_NOP,EP_ISO,CNF_4ISO3ISO,2},
	{EP_NUL,EP_NUL,EP_NUL,EP_NUL,EP_NUL,EP_NUL,EP_NUL,EP_NUL,EP_ISO,EP_ISO,EP_ISO,EP_ISO,EP_ISO,EP_ISO,EP_NUL,EP_NUL,CNF_3ISO3ISO,2},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}       // EOL element
};

// string description of chosen config
char *conf_str[]=
{
	"4 Interrupt IN + 3 Isochron OUT",
	"3 Interrupt IN + 3 Isochron OUT",
	"4 Isochron IN + 3 Isochron OUT",
	"3 Isochron IN + 3 Isochron OUT"
};


/*************************************************/
/* function called to probe a new plugged device */
/*************************************************/
static int hfc_usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev= interface_to_usbdev(intf);
	hfcusb_data *context;
	struct usb_host_interface *iface = intf->cur_altsetting;
	struct usb_host_interface *iface_used = NULL;
	struct usb_host_endpoint *ep;
	int ifnum = iface->desc.bInterfaceNumber;
	int i, idx, alt_idx, probe_alt_setting, vend_idx, cfg_used, *vcf, attr, cfg_found, cidx, ep_addr;
	int cmptbl[16],small_match,iso_packet_size,packet_size,alt_used=0;

//        usb_show_device(dev);
//	usb_show_device_descriptor(&dev->descriptor);
//	usb_show_interface_descriptor(&iface->desc);
	vend_idx=0xffff;
	for(i=0;vdata[i].vendor;i++) {
		if (le16_to_cpu(dev->descriptor.idVendor) == vdata[i].vendor && 
		    le16_to_cpu(dev->descriptor.idProduct) == vdata[i].prod_id)
			vend_idx = i;
	}
	

#ifdef VERBOSE_USB_DEBUG	
	printk(KERN_INFO "HFC-USB: probing interface(%d) actalt(%d) minor(%d)\n",
		ifnum, iface->desc.bAlternateSetting, intf->minor);
#endif

	if (vend_idx != 0xffff) {
#ifdef VERBOSE_USB_DEBUG
		printk(KERN_INFO "HFC-USB: found vendor idx:%d  name:%s\n",vend_idx,vdata[vend_idx].vend_name);
#endif
		/* if vendor and product ID is OK, start probing a matching alternate setting ... */
		alt_idx = 0;
		small_match=0xffff;
		// default settings
		iso_packet_size=16;
		packet_size=64;

		while (alt_idx < intf->num_altsetting) {
			iface = intf->altsetting + alt_idx;
			probe_alt_setting = iface->desc.bAlternateSetting;
			cfg_used=0;

#ifdef VERBOSE_USB_DEBUG
			printk(KERN_INFO "HFC-USB: test alt_setting %d\n", probe_alt_setting);
#endif
			// check for config EOL element
			while (validconf[cfg_used][0]) {
				cfg_found=TRUE;
				vcf=validconf[cfg_used];
				ep = iface->endpoint;	/* first endpoint descriptor */

#ifdef VERBOSE_USB_DEBUG
				printk(KERN_INFO "HFC-USB: (if=%d alt=%d cfg_used=%d)\n",
					ifnum, probe_alt_setting, cfg_used);
#endif
				// copy table
				memcpy(cmptbl,vcf,16*sizeof(int));

				// check for all endpoints in this alternate setting
				for (i=0; i < iface->desc.bNumEndpoints; i++) {
					ep_addr = ep->desc.bEndpointAddress;
					idx = ((ep_addr & 0x7f)-1)*2;	/* get endpoint base */
					if (ep_addr & 0x80)
						idx++;
					attr = ep->desc.bmAttributes;

					if (cmptbl[idx] == EP_NUL) {
						printk(KERN_INFO "HFC-USB: cfg_found=FALSE in idx:%d  attr:%d  cmptbl[%d]:%d\n",
							idx, attr, idx, cmptbl[idx]);
						cfg_found = FALSE;
					}

					if (attr == USB_ENDPOINT_XFER_INT && cmptbl[idx] == EP_INT)
						cmptbl[idx] = EP_NUL;
					if (attr == USB_ENDPOINT_XFER_BULK && cmptbl[idx] == EP_BLK)
						cmptbl[idx] = EP_NUL;
					if (attr == USB_ENDPOINT_XFER_ISOC && cmptbl[idx] == EP_ISO)
						cmptbl[idx] = EP_NUL;

					// check if all INT endpoints match minimum interval
					if (attr == USB_ENDPOINT_XFER_INT && ep->desc.bInterval < vcf[17]) {
#ifdef VERBOSE_USB_DEBUG
						if (cfg_found)
							printk(KERN_INFO "HFC-USB: Interrupt Endpoint interval < %d found - skipping config\n",
								vcf[17]);
#endif
						cfg_found = FALSE;
					}

					ep++;
				}

				for (i = 0; i < 16; i++) {
					// printk(KERN_INFO "HFC-USB: cmptbl[%d]:%d\n", i, cmptbl[i]);

					// all entries must be EP_NOP or EP_NUL for a valid config
					if (cmptbl[i] != EP_NOP && cmptbl[i] != EP_NUL)
						cfg_found = FALSE;
				}

				// we check for smallest match, to provide configuration priority
				// configurations with smaller index have higher priority
				if (cfg_found) {
					if (cfg_used < small_match) {
						small_match = cfg_used;
						alt_used = probe_alt_setting;
						iface_used = iface;
					}
#ifdef VERBOSE_USB_DEBUG
					printk(KERN_INFO "HFC-USB: small_match=%x %x\n", small_match, alt_used);
#endif
				}

				cfg_used++;
			}

			alt_idx++;
		}		/* (alt_idx < intf->num_altsetting) */
#ifdef VERBOSE_USB_DEBUG
		printk(KERN_INFO "HFC-USB: final small_match=%x alt_used=%x\n",small_match, alt_used);
#endif
		// yiipiee, we found a valid config
		if (small_match != 0xffff) {
			iface = iface_used;

			if (!(context = kmalloc(sizeof(hfcusb_data), GFP_KERNEL)))
				return(-ENOMEM);  /* got no mem */
			memset(context, 0, sizeof(hfcusb_data));	/* clear the structure */

			ep = iface->endpoint;	/* first endpoint descriptor */
			vcf = validconf[small_match];

			for (i = 0; i < iface->desc.bNumEndpoints; i++) {
				ep_addr = ep->desc.bEndpointAddress;
				idx = ((ep_addr & 0x7f)-1)*2;	/* get endpoint base */
				if (ep_addr & 0x80)
					idx++;
				cidx = idx & 7;
				attr = ep->desc.bmAttributes;

				// only initialize used endpoints
				if (vcf[idx] != EP_NOP && vcf[idx] != EP_NUL) {
					switch (attr) {
						case USB_ENDPOINT_XFER_INT:
							context->fifos[cidx].pipe = usb_rcvintpipe(dev, ep->desc.bEndpointAddress);
							context->fifos[cidx].usb_transfer_mode = USB_INT;
							packet_size = le16_to_cpu(ep->desc.wMaxPacketSize); // remember max packet size
#ifdef VERBOSE_USB_DEBUG
							printk (KERN_INFO "HFC-USB: Interrupt-In Endpoint found %d ms(idx:%d cidx:%d)!\n",
								ep->desc.bInterval, idx, cidx);
#endif
							break;
						case USB_ENDPOINT_XFER_BULK:
							if (ep_addr & 0x80)
								context->fifos[cidx].pipe = usb_rcvbulkpipe(dev, ep->desc.bEndpointAddress);
							else
								context->fifos[cidx].pipe = usb_sndbulkpipe(dev, ep->desc.bEndpointAddress);
							context->fifos[cidx].usb_transfer_mode = USB_BULK;
							packet_size = le16_to_cpu(ep->desc.wMaxPacketSize); // remember max packet size
#ifdef VERBOSE_USB_DEBUG
							printk (KERN_INFO "HFC-USB: Bulk Endpoint found (idx:%d cidx:%d)!\n",
								idx, cidx);
#endif
							break;
						case USB_ENDPOINT_XFER_ISOC:
							if (ep_addr & 0x80)
								context->fifos[cidx].pipe = usb_rcvisocpipe(dev, ep->desc.bEndpointAddress);
							else
								context->fifos[cidx].pipe = usb_sndisocpipe(dev, ep->desc.bEndpointAddress);
							context->fifos[cidx].usb_transfer_mode = USB_ISOC;
							iso_packet_size = le16_to_cpu(ep->desc.wMaxPacketSize); // remember max packet size
#ifdef VERBOSE_USB_DEBUG
							printk (KERN_INFO "HFC-USB: ISO Endpoint found (idx:%d cidx:%d)!\n",
								idx, cidx);
#endif
							break;
						default:
							context->fifos[cidx].pipe = 0;	/* reset data */
					}	/* switch attribute */

					if (context->fifos[cidx].pipe) {
						context->fifos[cidx].fifonum = cidx;
						context->fifos[cidx].hfc = context;
						context->fifos[cidx].usb_packet_maxlen = le16_to_cpu(ep->desc.wMaxPacketSize);
						context->fifos[cidx].intervall = ep->desc.bInterval;
						context->fifos[cidx].skbuff = NULL;
#ifdef VERBOSE_USB_DEBUG
						printk (KERN_INFO "HFC-USB: fifo%d pktlen %d interval %d\n",
							context->fifos[cidx].fifonum,
							context->fifos[cidx].usb_packet_maxlen,
							context->fifos[cidx].intervall);
#endif
					}
				}

				ep++;
			}

			// now share our luck
			context->dev = dev;						/* save device */
			context->if_used = ifnum;					/* save used interface */
			context->alt_used = alt_used;					/* and alternate config */
			context->ctrl_paksize = dev->descriptor.bMaxPacketSize0;	/* control size */
			context->cfg_used=vcf[16];					// store used config
			context->vend_idx=vend_idx;					// store found vendor
			context->packet_size=packet_size;
			context->iso_packet_size=iso_packet_size;

			/* create the control pipes needed for register access */
			context->ctrl_in_pipe = usb_rcvctrlpipe(context->dev, 0);
			context->ctrl_out_pipe = usb_sndctrlpipe(context->dev, 0);
			context->ctrl_urb = usb_alloc_urb(0, GFP_KERNEL);

			printk(KERN_INFO "HFC-USB: detected \"%s\" configuration: %s (if=%d alt=%d)\n",
				vdata[vend_idx].vend_name, conf_str[small_match], context->if_used, context->alt_used);

			/* init the chip and register the driver */
			if (usb_init(context))
			{
				if (context->ctrl_urb) {
					usb_unlink_urb(context->ctrl_urb);
					usb_free_urb(context->ctrl_urb);
					context->ctrl_urb = NULL;
				}
				kfree(context);
				return(-EIO);
			}
			usb_set_intfdata(intf, context);
			return(0);
		} 
	}
	return(-EIO);
}

/****************************************************/
/* function called when an active device is removed */
/****************************************************/
static void hfc_usb_disconnect(struct usb_interface *intf)
{
	hfcusb_data *context = usb_get_intfdata(intf);
	int i;

	printk(KERN_INFO "HFC-USB: device disconnect\n");
	
	usb_set_intfdata(intf, NULL);
	if (!context)
		return;
	if (timer_pending(&context->t3_timer))
		del_timer(&context->t3_timer);
	if (timer_pending(&context->t4_timer))
		del_timer(&context->t4_timer);
	if (timer_pending(&context->led_timer))
		del_timer(&context->led_timer);

	hisax_unregister(&context->d_if);

	/* tell all fifos to terminate */
	for(i = 0; i < HFCUSB_NUM_FIFOS; i++) {
		if(context->fifos[i].usb_transfer_mode == USB_ISOC) {
			if(context->fifos[i].active > 0) {
	    			stop_isoc_chain(&context->fifos[i]);
#ifdef VERBOSE_USB_DEBUG
		    		printk (KERN_INFO "HFC-USB: hfc_usb_disconnect: stopping ISOC chain Fifo no %i\n", i);
#endif
 			}
		} else {
			if(context->fifos[i].active > 0) {
				context->fifos[i].active = 0;
#ifdef VERBOSE_USB_DEBUG
				printk (KERN_INFO "HFC-USB: hfc_usb_disconnect: unlinking URB for Fifo no %i\n", i);
#endif
			}
			if (context->fifos[i].urb) {
				usb_unlink_urb(context->fifos[i].urb);
				usb_free_urb(context->fifos[i].urb);
				context->fifos[i].urb = NULL;
			}
		}
		context->fifos[i].active = 0;
	}
	if (context->ctrl_urb) {
		usb_unlink_urb(context->ctrl_urb);
		usb_free_urb(context->ctrl_urb);
		context->ctrl_urb = NULL;
	}
	kfree(context);		/* free our structure again */
}				/* hfc_usb_disconnect */


/************************************/
/* our driver information structure */
/************************************/
static struct usb_driver hfc_drv = {
	.owner =	THIS_MODULE,
	.name =		"hfc_usb",
	.id_table = 	hfc_usb_idtab,
	.probe =	hfc_usb_probe,
	.disconnect =	hfc_usb_disconnect,
};

static void __exit hfc_usb_exit(void)
{
#ifdef VERBOSE_USB_DEBUG
	printk ("HFC-USB: calling \"hfc_usb_exit\" ...\n");
#endif
	usb_deregister(&hfc_drv);	/* release our driver */
	printk(KERN_INFO "HFC-USB module removed\n");
}

static int __init hfc_usb_init(void)
{
	printk ("HFC-USB: driver module revision %s loaded\n", hfcusb_revision);

	if(usb_register(&hfc_drv))
	{
		printk(KERN_INFO "HFC-USB: Unable to register HFC-USB module at usb stack\n");
		return(-1);		   /* unable to register */
	}
	return(0);
}

module_init(hfc_usb_init);
module_exit(hfc_usb_exit);
