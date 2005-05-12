/* 
 *  FiberChannel transport specific attributes exported to sysfs.
 *
 *  Copyright (c) 2003 Silicon Graphics, Inc.  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/module.h>
#include <linux/init.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_transport.h>
#include <scsi/scsi_transport_fc.h>
#include "scsi_priv.h"

#define FC_PRINTK(x, l, f, a...)	printk(l "scsi(%d:%d:%d:%d): " f, (x)->host->host_no, (x)->channel, (x)->id, (x)->lun , ##a)

/*
 * Redefine so that we can have same named attributes in the
 * sdev/starget/host objects.
 */
#define FC_CLASS_DEVICE_ATTR(_prefix,_name,_mode,_show,_store)		\
struct class_device_attribute class_device_attr_##_prefix##_##_name = 	\
	__ATTR(_name,_mode,_show,_store)

#define fc_enum_name_search(title, table_type, table)			\
static const char *get_fc_##title##_name(enum table_type table_key)	\
{									\
	int i;								\
	char *name = NULL;						\
									\
	for (i = 0; i < sizeof(table)/sizeof(table[0]); i++) {		\
		if (table[i].value == table_key) {			\
			name = table[i].name;				\
			break;						\
		}							\
	}								\
	return name;							\
}

#define fc_enum_name_match(title, table_type, table)			\
static int get_fc_##title##_match(const char *table_key,		\
		enum table_type *value)					\
{									\
	int i;								\
									\
	for (i = 0; i < sizeof(table)/sizeof(table[0]); i++) {		\
		if (strncmp(table_key, table[i].name,			\
				table[i].matchlen) == 0) {		\
			*value = table[i].value;			\
			return 0; /* success */				\
		}							\
	}								\
	return 1; /* failure */						\
}


/* Convert fc_port_type values to ascii string name */
static struct {
	enum fc_port_type	value;
	char			*name;
} fc_port_type_names[] = {
	{ FC_PORTTYPE_UNKNOWN,		"Unknown" },
	{ FC_PORTTYPE_OTHER,		"Other" },
	{ FC_PORTTYPE_NOTPRESENT,	"Not Present" },
	{ FC_PORTTYPE_NPORT,	"NPort (fabric via point-to-point)" },
	{ FC_PORTTYPE_NLPORT,	"NLPort (fabric via loop)" },
	{ FC_PORTTYPE_LPORT,	"LPort (private loop)" },
	{ FC_PORTTYPE_PTP,	"Point-To-Point (direct nport connection" },
};
fc_enum_name_search(port_type, fc_port_type, fc_port_type_names)
#define FC_PORTTYPE_MAX_NAMELEN		50


/* Convert fc_port_state values to ascii string name */
static struct {
	enum fc_port_state	value;
	char			*name;
} fc_port_state_names[] = {
	{ FC_PORTSTATE_UNKNOWN,		"Unknown" },
	{ FC_PORTSTATE_ONLINE,		"Online" },
	{ FC_PORTSTATE_OFFLINE,		"Offline" },
	{ FC_PORTSTATE_BYPASSED,	"Bypassed" },
	{ FC_PORTSTATE_DIAGNOSTICS,	"Diagnostics" },
	{ FC_PORTSTATE_LINKDOWN,	"Linkdown" },
	{ FC_PORTSTATE_ERROR,		"Error" },
	{ FC_PORTSTATE_LOOPBACK,	"Loopback" },
};
fc_enum_name_search(port_state, fc_port_state, fc_port_state_names)
#define FC_PORTSTATE_MAX_NAMELEN	20


/* Convert fc_tgtid_binding_type values to ascii string name */
static struct {
	enum fc_tgtid_binding_type	value;
	char				*name;
	int				matchlen;
} fc_tgtid_binding_type_names[] = {
	{ FC_TGTID_BIND_BY_WWPN, "wwpn (World Wide Port Name)", 4 },
	{ FC_TGTID_BIND_BY_WWNN, "wwnn (World Wide Node Name)", 4 },
	{ FC_TGTID_BIND_BY_ID, "fcportid (FC Address)", 8 },
};
fc_enum_name_search(tgtid_bind_type, fc_tgtid_binding_type,
		fc_tgtid_binding_type_names)
fc_enum_name_match(tgtid_bind_type, fc_tgtid_binding_type,
		fc_tgtid_binding_type_names)
#define FC_BINDTYPE_MAX_NAMELEN	30


#define fc_bitfield_name_search(title, table)			\
static ssize_t							\
get_fc_##title##_names(u32 table_key, char *buf)		\
{								\
	char *prefix = "";					\
	ssize_t len = 0;					\
	int i;							\
								\
	for (i = 0; i < sizeof(table)/sizeof(table[0]); i++) {	\
		if (table[i].value & table_key) {		\
			len += sprintf(buf + len, "%s%s",	\
				prefix, table[i].name);		\
			prefix = ", ";				\
		}						\
	}							\
	len += sprintf(buf + len, "\n");			\
	return len;						\
}


/* Convert fc_cos bit values to ascii string name */
static struct {
	u32 			value;
	char			*name;
} fc_cos_names[] = {
	{ FC_COS_CLASS1,	"Class 1" },
	{ FC_COS_CLASS2,	"Class 2" },
	{ FC_COS_CLASS3,	"Class 3" },
	{ FC_COS_CLASS4,	"Class 4" },
	{ FC_COS_CLASS6,	"Class 6" },
};
fc_bitfield_name_search(cos, fc_cos_names)


/* Convert fc_port_speed bit values to ascii string name */
static struct {
	u32 			value;
	char			*name;
} fc_port_speed_names[] = {
	{ FC_PORTSPEED_1GBIT,		"1 Gbit" },
	{ FC_PORTSPEED_2GBIT,		"2 Gbit" },
	{ FC_PORTSPEED_4GBIT,		"4 Gbit" },
	{ FC_PORTSPEED_10GBIT,		"10 Gbit" },
	{ FC_PORTSPEED_NOT_NEGOTIATED,	"Not Negotiated" },
};
fc_bitfield_name_search(port_speed, fc_port_speed_names)


static int
show_fc_fc4s (char *buf, u8 *fc4_list)
{
	int i, len=0;

	for (i = 0; i < FC_FC4_LIST_SIZE; i++, fc4_list++)
		len += sprintf(buf + len , "0x%02x ", *fc4_list);
	len += sprintf(buf + len, "\n");
	return len;
}



static void fc_timeout_blocked_host(void *data);
static void fc_timeout_blocked_tgt(void *data);

#define FC_STARGET_NUM_ATTRS 	4	/* increase this if you add attributes */
#define FC_STARGET_OTHER_ATTRS 	0	/* increase this if you add "always on"
					 * attributes */
#define FC_HOST_NUM_ATTRS	15

struct fc_internal {
	struct scsi_transport_template t;
	struct fc_function_template *f;
	/* The actual attributes */
	struct class_device_attribute private_starget_attrs[
						FC_STARGET_NUM_ATTRS];
	/* The array of null terminated pointers to attributes
	 * needed by scsi_sysfs.c */
	struct class_device_attribute *starget_attrs[
			FC_STARGET_NUM_ATTRS + FC_STARGET_OTHER_ATTRS + 1];

	struct class_device_attribute private_host_attrs[FC_HOST_NUM_ATTRS];
	struct class_device_attribute *host_attrs[FC_HOST_NUM_ATTRS + 1];
};

#define to_fc_internal(tmpl)	container_of(tmpl, struct fc_internal, t)

static int fc_add_target(struct device *dev)
{
	struct scsi_target *starget = to_scsi_target(dev);
	/* 
	 * Set default values easily detected by the midlayer as
	 * failure cases.  The scsi lldd is responsible for initializing
	 * all transport attributes to valid values per target.
	 */
	fc_starget_node_name(starget) = -1;
	fc_starget_port_name(starget) = -1;
	fc_starget_port_id(starget) = -1;
	fc_starget_dev_loss_tmo(starget) = -1;
	INIT_WORK(&fc_starget_dev_loss_work(starget),
		  fc_timeout_blocked_tgt, starget);
	return 0;
}

static int fc_remove_target(struct device *dev)
{
	struct scsi_target *starget = to_scsi_target(dev);
	/* Stop the target timer */
	if (cancel_delayed_work(&fc_starget_dev_loss_work(starget)))
		flush_scheduled_work();
	return 0;
}

static DECLARE_TRANSPORT_CLASS(fc_transport_class,
			       "fc_transport",
			       fc_add_target,
			       fc_remove_target,
			       NULL);

static int fc_add_host(struct device *dev)
{
	struct Scsi_Host *shost = dev_to_shost(dev);
	/* 
	 * Set default values easily detected by the midlayer as
	 * failure cases.  The scsi lldd is responsible for initializing
	 * all transport attributes to valid values per host.
	 */
	fc_host_node_name(shost) = -1;
	fc_host_port_name(shost) = -1;
	fc_host_supported_classes(shost) = FC_COS_UNSPECIFIED;
	memset(fc_host_supported_fc4s(shost), 0,
		sizeof(fc_host_supported_fc4s(shost)));
	memset(fc_host_symbolic_name(shost), 0,
		sizeof(fc_host_symbolic_name(shost)));
	fc_host_supported_speeds(shost) = FC_PORTSPEED_UNKNOWN;
	fc_host_maxframe_size(shost) = -1;
	memset(fc_host_hardware_version(shost), 0,
		sizeof(fc_host_hardware_version(shost)));
	memset(fc_host_firmware_version(shost), 0,
		sizeof(fc_host_firmware_version(shost)));
	memset(fc_host_serial_number(shost), 0,
		sizeof(fc_host_serial_number(shost)));
	memset(fc_host_opt_rom_version(shost), 0,
		sizeof(fc_host_opt_rom_version(shost)));
	memset(fc_host_driver_version(shost), 0,
		sizeof(fc_host_driver_version(shost)));

	fc_host_port_id(shost) = -1;
	fc_host_port_type(shost) = FC_PORTTYPE_UNKNOWN;
	fc_host_port_state(shost) = FC_PORTSTATE_UNKNOWN;
	memset(fc_host_active_fc4s(shost), 0,
		sizeof(fc_host_active_fc4s(shost)));
	fc_host_speed(shost) = FC_PORTSPEED_UNKNOWN;
	fc_host_fabric_name(shost) = -1;
 	fc_host_link_down_tmo(shost) = -1;

	fc_host_tgtid_bind_type(shost) = FC_TGTID_BIND_BY_WWPN;

	INIT_WORK(&fc_host_link_down_work(shost),
		  fc_timeout_blocked_host, shost);
	return 0;
}

static int fc_remove_host(struct device *dev)
{
	struct Scsi_Host *shost = dev_to_shost(dev);
	/* Stop the host timer */
	if (cancel_delayed_work(&fc_host_link_down_work(shost)))
		flush_scheduled_work();
	return 0;
}

static DECLARE_TRANSPORT_CLASS(fc_host_class,
			       "fc_host",
			       fc_add_host,
			       fc_remove_host,
			       NULL);

static __init int fc_transport_init(void)
{
	int error = transport_class_register(&fc_host_class);
	if (error)
		return error;
	return transport_class_register(&fc_transport_class);
}

static void __exit fc_transport_exit(void)
{
	transport_class_unregister(&fc_transport_class);
	transport_class_unregister(&fc_host_class);
}

/*
 * Remote Port (Target) Attribute Management
 */

#define fc_starget_show_function(field, format_string, cast)		\
static ssize_t								\
show_fc_starget_##field (struct class_device *cdev, char *buf)		\
{									\
	struct scsi_target *starget = transport_class_to_starget(cdev);	\
	struct Scsi_Host *shost = dev_to_shost(starget->dev.parent);	\
	struct fc_internal *i = to_fc_internal(shost->transportt);	\
	if (i->f->get_starget_##field)					\
		i->f->get_starget_##field(starget);			\
	return snprintf(buf, 20, format_string, 			\
		cast fc_starget_##field(starget)); 			\
}

#define fc_starget_store_function(field, format_string)			\
static ssize_t								\
store_fc_starget_##field(struct class_device *cdev, const char *buf,	\
			   size_t count)				\
{									\
	int val;							\
	struct scsi_target *starget = transport_class_to_starget(cdev);	\
	struct Scsi_Host *shost = dev_to_shost(starget->dev.parent);	\
	struct fc_internal *i = to_fc_internal(shost->transportt);	\
									\
	val = simple_strtoul(buf, NULL, 0);				\
	i->f->set_starget_##field(starget, val);			\
	return count;							\
}

#define fc_starget_rd_attr(field, format_string)			\
	fc_starget_show_function(field, format_string, )		\
static FC_CLASS_DEVICE_ATTR(starget, field, S_IRUGO,			\
			 show_fc_starget_##field, NULL)

#define fc_starget_rd_attr_cast(field, format_string, cast)		\
	fc_starget_show_function(field, format_string, (cast))		\
static FC_CLASS_DEVICE_ATTR(starget, field, S_IRUGO,			\
			  show_fc_starget_##field, NULL)

#define fc_starget_rw_attr(field, format_string)			\
	fc_starget_show_function(field, format_string, )		\
	fc_starget_store_function(field, format_string)			\
static FC_CLASS_DEVICE_ATTR(starget, field, S_IRUGO | S_IWUSR,		\
			show_fc_starget_##field,			\
			store_fc_starget_##field)

#define SETUP_STARGET_ATTRIBUTE_RD(field)				\
	i->private_starget_attrs[count] = class_device_attr_starget_##field; \
	i->private_starget_attrs[count].attr.mode = S_IRUGO;		\
	i->private_starget_attrs[count].store = NULL;			\
	i->starget_attrs[count] = &i->private_starget_attrs[count];	\
	if (i->f->show_starget_##field)					\
		count++

#define SETUP_STARGET_ATTRIBUTE_RW(field)				\
	i->private_starget_attrs[count] = class_device_attr_starget_##field; \
	if (!i->f->set_starget_##field) {				\
		i->private_starget_attrs[count].attr.mode = S_IRUGO;	\
		i->private_starget_attrs[count].store = NULL;		\
	}								\
	i->starget_attrs[count] = &i->private_starget_attrs[count];	\
	if (i->f->show_starget_##field)					\
		count++

/* The FC Tranport Remote Port (Target) Attributes: */
fc_starget_rd_attr_cast(node_name, "0x%llx\n", unsigned long long);
fc_starget_rd_attr_cast(port_name, "0x%llx\n", unsigned long long);
fc_starget_rd_attr(port_id, "0x%06x\n");
fc_starget_rw_attr(dev_loss_tmo, "%d\n");



/*
 * Host Attribute Management
 */

#define fc_host_show_function(field, format_string, sz, cast)		\
static ssize_t								\
show_fc_host_##field (struct class_device *cdev, char *buf)		\
{									\
	struct Scsi_Host *shost = transport_class_to_shost(cdev);	\
	struct fc_internal *i = to_fc_internal(shost->transportt);	\
	if (i->f->get_host_##field)					\
		i->f->get_host_##field(shost);				\
	return snprintf(buf, sz, format_string, cast fc_host_##field(shost)); \
}

#define fc_host_store_function(field, format_string)			\
static ssize_t								\
store_fc_host_##field(struct class_device *cdev, const char *buf,	\
			   size_t count)				\
{									\
	int val;							\
	struct Scsi_Host *shost = transport_class_to_shost(cdev);	\
	struct fc_internal *i = to_fc_internal(shost->transportt);	\
									\
	val = simple_strtoul(buf, NULL, 0);				\
	i->f->set_host_##field(shost, val);				\
	return count;							\
}

#define fc_host_rd_attr(field, format_string, sz)			\
	fc_host_show_function(field, format_string, sz, )		\
static FC_CLASS_DEVICE_ATTR(host, field, S_IRUGO,			\
			 show_fc_host_##field, NULL)

#define fc_host_rd_attr_cast(field, format_string, sz, cast)		\
	fc_host_show_function(field, format_string, sz, (cast))		\
static FC_CLASS_DEVICE_ATTR(host, field, S_IRUGO,			\
			  show_fc_host_##field, NULL)

#define fc_host_rw_attr(field, format_string, sz)			\
	fc_host_show_function(field, format_string, sz, )		\
	fc_host_store_function(field, format_string)			\
static FC_CLASS_DEVICE_ATTR(host, field, S_IRUGO | S_IWUSR,		\
			show_fc_host_##field,				\
			store_fc_host_##field)

#define fc_host_rd_enum_attr(title, maxlen)				\
static ssize_t								\
show_fc_host_##title (struct class_device *cdev, char *buf)		\
{									\
	struct Scsi_Host *shost = transport_class_to_shost(cdev);	\
	struct fc_internal *i = to_fc_internal(shost->transportt);	\
	const char *name;						\
	if (i->f->get_host_##title)					\
		i->f->get_host_##title(shost);				\
	name = get_fc_##title##_name(fc_host_##title(shost));		\
	if (!name)							\
		return -EINVAL;						\
	return snprintf(buf, maxlen, "%s\n", name);			\
}									\
static FC_CLASS_DEVICE_ATTR(host, title, S_IRUGO, show_fc_host_##title, NULL)

#define SETUP_HOST_ATTRIBUTE_RD(field)					\
	i->private_host_attrs[count] = class_device_attr_host_##field;	\
	i->private_host_attrs[count].attr.mode = S_IRUGO;		\
	i->private_host_attrs[count].store = NULL;			\
	i->host_attrs[count] = &i->private_host_attrs[count];		\
	if (i->f->show_host_##field)					\
		count++

#define SETUP_HOST_ATTRIBUTE_RW(field)					\
	i->private_host_attrs[count] = class_device_attr_host_##field;	\
	if (!i->f->set_host_##field) {					\
		i->private_host_attrs[count].attr.mode = S_IRUGO;	\
		i->private_host_attrs[count].store = NULL;		\
	}								\
	i->host_attrs[count] = &i->private_host_attrs[count];		\
	if (i->f->show_host_##field)					\
		count++


#define fc_private_host_show_function(field, format_string, sz, cast)	\
static ssize_t								\
show_fc_host_##field (struct class_device *cdev, char *buf)		\
{									\
	struct Scsi_Host *shost = transport_class_to_shost(cdev);	\
	return snprintf(buf, sz, format_string, cast fc_host_##field(shost)); \
}

#define fc_private_host_rd_attr(field, format_string, sz)		\
	fc_private_host_show_function(field, format_string, sz, )	\
static FC_CLASS_DEVICE_ATTR(host, field, S_IRUGO,			\
			 show_fc_host_##field, NULL)

#define fc_private_host_rd_attr_cast(field, format_string, sz, cast)	\
	fc_private_host_show_function(field, format_string, sz, (cast)) \
static FC_CLASS_DEVICE_ATTR(host, field, S_IRUGO,			\
			  show_fc_host_##field, NULL)

#define SETUP_PRIVATE_HOST_ATTRIBUTE_RD(field)			\
	i->private_host_attrs[count] = class_device_attr_host_##field;	\
	i->private_host_attrs[count].attr.mode = S_IRUGO;		\
	i->private_host_attrs[count].store = NULL;			\
	i->host_attrs[count] = &i->private_host_attrs[count];		\
	count++

#define SETUP_PRIVATE_HOST_ATTRIBUTE_RW(field)			\
	i->private_host_attrs[count] = class_device_attr_host_##field;	\
	i->host_attrs[count] = &i->private_host_attrs[count];		\
	count++


/* Fixed Host Attributes */

static ssize_t
show_fc_host_supported_classes (struct class_device *cdev, char *buf)
{
	struct Scsi_Host *shost = transport_class_to_shost(cdev);

	if (fc_host_supported_classes(shost) == FC_COS_UNSPECIFIED)
		return snprintf(buf, 20, "unspecified\n");

	return get_fc_cos_names(fc_host_supported_classes(shost), buf);
}
static FC_CLASS_DEVICE_ATTR(host, supported_classes, S_IRUGO,
		show_fc_host_supported_classes, NULL);

static ssize_t
show_fc_host_supported_fc4s (struct class_device *cdev, char *buf)
{
	struct Scsi_Host *shost = transport_class_to_shost(cdev);
	return (ssize_t)show_fc_fc4s(buf, fc_host_supported_fc4s(shost));
}
static FC_CLASS_DEVICE_ATTR(host, supported_fc4s, S_IRUGO,
		show_fc_host_supported_fc4s, NULL);

static ssize_t
show_fc_host_supported_speeds (struct class_device *cdev, char *buf)
{
	struct Scsi_Host *shost = transport_class_to_shost(cdev);

	if (fc_host_supported_speeds(shost) == FC_PORTSPEED_UNKNOWN)
		return snprintf(buf, 20, "unknown\n");

	return get_fc_port_speed_names(fc_host_supported_speeds(shost), buf);
}
static FC_CLASS_DEVICE_ATTR(host, supported_speeds, S_IRUGO,
		show_fc_host_supported_speeds, NULL);


fc_private_host_rd_attr_cast(node_name, "0x%llx\n", 20, unsigned long long);
fc_private_host_rd_attr_cast(port_name, "0x%llx\n", 20, unsigned long long);
fc_private_host_rd_attr(symbolic_name, "%s\n", (FC_SYMBOLIC_NAME_SIZE +1));
fc_private_host_rd_attr(maxframe_size, "%u bytes\n", 20);
fc_private_host_rd_attr(hardware_version, "%s\n", (FC_VERSION_STRING_SIZE +1));
fc_private_host_rd_attr(firmware_version, "%s\n", (FC_VERSION_STRING_SIZE +1));
fc_private_host_rd_attr(serial_number, "%s\n", (FC_SERIAL_NUMBER_SIZE +1));
fc_private_host_rd_attr(opt_rom_version, "%s\n", (FC_VERSION_STRING_SIZE +1));
fc_private_host_rd_attr(driver_version, "%s\n", (FC_VERSION_STRING_SIZE +1));


/* Dynamic Host Attributes */

static ssize_t
show_fc_host_active_fc4s (struct class_device *cdev, char *buf)
{
	struct Scsi_Host *shost = transport_class_to_shost(cdev);
	struct fc_internal *i = to_fc_internal(shost->transportt);

	if (i->f->get_host_active_fc4s)
		i->f->get_host_active_fc4s(shost);

	return (ssize_t)show_fc_fc4s(buf, fc_host_active_fc4s(shost));
}
static FC_CLASS_DEVICE_ATTR(host, active_fc4s, S_IRUGO,
		show_fc_host_active_fc4s, NULL);

static ssize_t
show_fc_host_speed (struct class_device *cdev, char *buf)
{
	struct Scsi_Host *shost = transport_class_to_shost(cdev);
	struct fc_internal *i = to_fc_internal(shost->transportt);

	if (i->f->get_host_speed)
		i->f->get_host_speed(shost);

	if (fc_host_speed(shost) == FC_PORTSPEED_UNKNOWN)
		return snprintf(buf, 20, "unknown\n");

	return get_fc_port_speed_names(fc_host_speed(shost), buf);
}
static FC_CLASS_DEVICE_ATTR(host, speed, S_IRUGO,
		show_fc_host_speed, NULL);


fc_host_rd_attr(port_id, "0x%06x\n", 20);
fc_host_rd_enum_attr(port_type, FC_PORTTYPE_MAX_NAMELEN);
fc_host_rd_enum_attr(port_state, FC_PORTSTATE_MAX_NAMELEN);
fc_host_rd_attr_cast(fabric_name, "0x%llx\n", 20, unsigned long long);
fc_host_rw_attr(link_down_tmo, "%d\n", 20);


/* Private Host Attributes */

static ssize_t
show_fc_private_host_tgtid_bind_type(struct class_device *cdev, char *buf)
{
	struct Scsi_Host *shost = transport_class_to_shost(cdev);
	const char *name;

	name = get_fc_tgtid_bind_type_name(fc_host_tgtid_bind_type(shost));
	if (!name)
		return -EINVAL;
	return snprintf(buf, FC_BINDTYPE_MAX_NAMELEN, "%s\n", name);
}

static ssize_t
store_fc_private_host_tgtid_bind_type(struct class_device *cdev,
	const char *buf, size_t count)
{
	struct Scsi_Host *shost = transport_class_to_shost(cdev);
	enum fc_tgtid_binding_type val;

	if (get_fc_tgtid_bind_type_match(buf, &val))
		return -EINVAL;
	fc_host_tgtid_bind_type(shost) = val;
	return count;
}

static FC_CLASS_DEVICE_ATTR(host, tgtid_bind_type, S_IRUGO | S_IWUSR,
			show_fc_private_host_tgtid_bind_type,
			store_fc_private_host_tgtid_bind_type);

/*
 * Host Statistics Management
 */

/* Show a given an attribute in the statistics group */
static ssize_t
fc_stat_show(const struct class_device *cdev, char *buf, unsigned long offset)
{
	struct Scsi_Host *shost = transport_class_to_shost(cdev);
	struct fc_internal *i = to_fc_internal(shost->transportt);
	struct fc_host_statistics *stats;
	ssize_t ret = -ENOENT;

	if (offset > sizeof(struct fc_host_statistics) ||
	    offset % sizeof(u64) != 0)
		WARN_ON(1);

	if (i->f->get_fc_host_stats) {
		stats = (i->f->get_fc_host_stats)(shost);
		if (stats)
			ret = snprintf(buf, 20, "0x%llx\n",
			      (unsigned long long)*(u64 *)(((u8 *) stats) + offset));
	}
	return ret;
}


/* generate a read-only statistics attribute */
#define fc_host_statistic(name)						\
static ssize_t show_fcstat_##name(struct class_device *cd, char *buf) 	\
{									\
	return fc_stat_show(cd, buf, 					\
			    offsetof(struct fc_host_statistics, name));	\
}									\
static FC_CLASS_DEVICE_ATTR(host, name, S_IRUGO, show_fcstat_##name, NULL)

fc_host_statistic(seconds_since_last_reset);
fc_host_statistic(tx_frames);
fc_host_statistic(tx_words);
fc_host_statistic(rx_frames);
fc_host_statistic(rx_words);
fc_host_statistic(lip_count);
fc_host_statistic(nos_count);
fc_host_statistic(error_frames);
fc_host_statistic(dumped_frames);
fc_host_statistic(link_failure_count);
fc_host_statistic(loss_of_sync_count);
fc_host_statistic(loss_of_signal_count);
fc_host_statistic(prim_seq_protocol_err_count);
fc_host_statistic(invalid_tx_word_count);
fc_host_statistic(invalid_crc_count);
fc_host_statistic(fcp_input_requests);
fc_host_statistic(fcp_output_requests);
fc_host_statistic(fcp_control_requests);
fc_host_statistic(fcp_input_megabytes);
fc_host_statistic(fcp_output_megabytes);

static ssize_t
fc_reset_statistics(struct class_device *cdev, const char *buf,
			   size_t count)
{
	struct Scsi_Host *shost = transport_class_to_shost(cdev);
	struct fc_internal *i = to_fc_internal(shost->transportt);

	/* ignore any data value written to the attribute */
	if (i->f->reset_fc_host_stats) {
		i->f->reset_fc_host_stats(shost);
		return count;
	}

	return -ENOENT;
}
static FC_CLASS_DEVICE_ATTR(host, reset_statistics, S_IWUSR, NULL,
				fc_reset_statistics);


static struct attribute *fc_statistics_attrs[] = {
	&class_device_attr_host_seconds_since_last_reset.attr,
	&class_device_attr_host_tx_frames.attr,
	&class_device_attr_host_tx_words.attr,
	&class_device_attr_host_rx_frames.attr,
	&class_device_attr_host_rx_words.attr,
	&class_device_attr_host_lip_count.attr,
	&class_device_attr_host_nos_count.attr,
	&class_device_attr_host_error_frames.attr,
	&class_device_attr_host_dumped_frames.attr,
	&class_device_attr_host_link_failure_count.attr,
	&class_device_attr_host_loss_of_sync_count.attr,
	&class_device_attr_host_loss_of_signal_count.attr,
	&class_device_attr_host_prim_seq_protocol_err_count.attr,
	&class_device_attr_host_invalid_tx_word_count.attr,
	&class_device_attr_host_invalid_crc_count.attr,
	&class_device_attr_host_fcp_input_requests.attr,
	&class_device_attr_host_fcp_output_requests.attr,
	&class_device_attr_host_fcp_control_requests.attr,
	&class_device_attr_host_fcp_input_megabytes.attr,
	&class_device_attr_host_fcp_output_megabytes.attr,
	&class_device_attr_host_reset_statistics.attr,
	NULL
};

static struct attribute_group fc_statistics_group = {
	.name = "statistics",
	.attrs = fc_statistics_attrs,
};

static int fc_host_match(struct attribute_container *cont,
			  struct device *dev)
{
	struct Scsi_Host *shost;
	struct fc_internal *i;

	if (!scsi_is_host_device(dev))
		return 0;

	shost = dev_to_shost(dev);
	if (!shost->transportt  || shost->transportt->host_attrs.class
	    != &fc_host_class.class)
		return 0;

	i = to_fc_internal(shost->transportt);
	
	return &i->t.host_attrs == cont;
}

static int fc_target_match(struct attribute_container *cont,
			    struct device *dev)
{
	struct Scsi_Host *shost;
	struct fc_internal *i;

	if (!scsi_is_target_device(dev))
		return 0;

	shost = dev_to_shost(dev->parent);
	if (!shost->transportt  || shost->transportt->host_attrs.class
	    != &fc_host_class.class)
		return 0;

	i = to_fc_internal(shost->transportt);
	
	return &i->t.target_attrs == cont;
}


struct scsi_transport_template *
fc_attach_transport(struct fc_function_template *ft)
{
	struct fc_internal *i = kmalloc(sizeof(struct fc_internal),
					GFP_KERNEL);
	int count = 0;

	if (unlikely(!i))
		return NULL;

	memset(i, 0, sizeof(struct fc_internal));

	i->t.target_attrs.attrs = &i->starget_attrs[0];
	i->t.target_attrs.class = &fc_transport_class.class;
	i->t.target_attrs.match = fc_target_match;
	attribute_container_register(&i->t.target_attrs);
	i->t.target_size = sizeof(struct fc_starget_attrs);

	i->t.host_attrs.attrs = &i->host_attrs[0];
	i->t.host_attrs.class = &fc_host_class.class;
	i->t.host_attrs.match = fc_host_match;
	attribute_container_register(&i->t.host_attrs);
	i->t.host_size = sizeof(struct fc_host_attrs);

	if (ft->get_fc_host_stats)
		i->t.host_statistics = &fc_statistics_group;

	i->f = ft;

	
	/*
	 * setup remote port (target) attributes
	 */
	SETUP_STARGET_ATTRIBUTE_RD(port_id);
	SETUP_STARGET_ATTRIBUTE_RD(port_name);
	SETUP_STARGET_ATTRIBUTE_RD(node_name);
	SETUP_STARGET_ATTRIBUTE_RW(dev_loss_tmo);

	BUG_ON(count > FC_STARGET_NUM_ATTRS);

	/* Setup the always-on attributes here */

	i->starget_attrs[count] = NULL;


	/* setup host attributes */
	count=0;
	SETUP_HOST_ATTRIBUTE_RD(node_name);
	SETUP_HOST_ATTRIBUTE_RD(port_name);
	SETUP_HOST_ATTRIBUTE_RD(supported_classes);
	SETUP_HOST_ATTRIBUTE_RD(supported_fc4s);
	SETUP_HOST_ATTRIBUTE_RD(symbolic_name);
	SETUP_HOST_ATTRIBUTE_RD(supported_speeds);
	SETUP_HOST_ATTRIBUTE_RD(maxframe_size);
	SETUP_HOST_ATTRIBUTE_RD(hardware_version);
	SETUP_HOST_ATTRIBUTE_RD(firmware_version);
	SETUP_HOST_ATTRIBUTE_RD(serial_number);
	SETUP_HOST_ATTRIBUTE_RD(opt_rom_version);
	SETUP_HOST_ATTRIBUTE_RD(driver_version);

	SETUP_HOST_ATTRIBUTE_RD(port_id);
	SETUP_HOST_ATTRIBUTE_RD(port_type);
	SETUP_HOST_ATTRIBUTE_RD(port_state);
	SETUP_HOST_ATTRIBUTE_RD(active_fc4s);
	SETUP_HOST_ATTRIBUTE_RD(speed);
	SETUP_HOST_ATTRIBUTE_RD(fabric_name);
	SETUP_HOST_ATTRIBUTE_RW(link_down_tmo);

	/* Transport-managed attributes */
	SETUP_PRIVATE_HOST_ATTRIBUTE_RW(tgtid_bind_type);

	BUG_ON(count > FC_HOST_NUM_ATTRS);

	i->host_attrs[count] = NULL;


	return &i->t;
}
EXPORT_SYMBOL(fc_attach_transport);

void fc_release_transport(struct scsi_transport_template *t)
{
	struct fc_internal *i = to_fc_internal(t);

	attribute_container_unregister(&i->t.target_attrs);
	attribute_container_unregister(&i->t.host_attrs);

	kfree(i);
}
EXPORT_SYMBOL(fc_release_transport);



/**
 * fc_device_block - called by target functions to block a scsi device
 * @dev:	scsi device
 * @data:	unused
 **/
static void fc_device_block(struct scsi_device *sdev, void *data)
{
	scsi_internal_device_block(sdev);
}

/**
 * fc_device_unblock - called by target functions to unblock a scsi device
 * @dev:	scsi device
 * @data:	unused
 **/
static void fc_device_unblock(struct scsi_device *sdev, void *data)
{
	scsi_internal_device_unblock(sdev);
}

/**
 * fc_timeout_blocked_tgt - Timeout handler for blocked scsi targets
 *			 that fail to recover in the alloted time.
 * @data:	scsi target that failed to reappear in the alloted time.
 **/
static void fc_timeout_blocked_tgt(void  *data)
{
	struct scsi_target *starget = (struct scsi_target *)data;

	dev_printk(KERN_ERR, &starget->dev, 
		"blocked target time out: target resuming\n");

	/* 
	 * set the device going again ... if the scsi lld didn't
	 * unblock this device, then IO errors will probably
	 * result if the host still isn't ready.
	 */
	starget_for_each_device(starget, NULL, fc_device_unblock);
}

/**
 * fc_target_block - block a target by temporarily putting all its scsi devices
 *		into the SDEV_BLOCK state.
 * @starget:	scsi target managed by this fc scsi lldd.
 *
 * scsi lldd's with a FC transport call this routine to temporarily stop all
 * scsi commands to all devices managed by this scsi target.  Called 
 * from interrupt or normal process context.
 *
 * Returns zero if successful or error if not
 *
 * Notes:       
 *	The timeout and timer types are extracted from the fc transport 
 *	attributes from the caller's target pointer.  This routine assumes no
 *	locks are held on entry.
 **/
int
fc_target_block(struct scsi_target *starget)
{
	int timeout = fc_starget_dev_loss_tmo(starget);
	struct work_struct *work = &fc_starget_dev_loss_work(starget);

	if (timeout < 0 || timeout > SCSI_DEVICE_BLOCK_MAX_TIMEOUT)
		return -EINVAL;

	starget_for_each_device(starget, NULL, fc_device_block);

	/* The scsi lld blocks this target for the timeout period only. */
	schedule_delayed_work(work, timeout * HZ);

	return 0;
}
EXPORT_SYMBOL(fc_target_block);

/**
 * fc_target_unblock - unblock a target following a fc_target_block request.
 * @starget:	scsi target managed by this fc scsi lldd.	
 *
 * scsi lld's with a FC transport call this routine to restart IO to all 
 * devices associated with the caller's scsi target following a fc_target_block
 * request.  Called from interrupt or normal process context.
 *
 * Notes:       
 *	This routine assumes no locks are held on entry.
 **/
void
fc_target_unblock(struct scsi_target *starget)
{
	/* 
	 * Stop the target timer first. Take no action on the del_timer
	 * failure as the state machine state change will validate the
	 * transaction. 
	 */
	if (cancel_delayed_work(&fc_starget_dev_loss_work(starget)))
		flush_scheduled_work();

	starget_for_each_device(starget, NULL, fc_device_unblock);
}
EXPORT_SYMBOL(fc_target_unblock);

/**
 * fc_timeout_blocked_host - Timeout handler for blocked scsi hosts
 *			 that fail to recover in the alloted time.
 * @data:	scsi host that failed to recover its devices in the alloted
 *		time.
 **/
static void fc_timeout_blocked_host(void  *data)
{
	struct Scsi_Host *shost = (struct Scsi_Host *)data;
	struct scsi_device *sdev;

	dev_printk(KERN_ERR, &shost->shost_gendev, 
		"blocked host time out: host resuming\n");

	shost_for_each_device(sdev, shost) {
		/* 
		 * set the device going again ... if the scsi lld didn't
		 * unblock this device, then IO errors will probably
		 * result if the host still isn't ready.
		 */
		scsi_internal_device_unblock(sdev);
	}
}

/**
 * fc_host_block - block all scsi devices managed by the calling host temporarily 
 *		by putting each device in the SDEV_BLOCK state.
 * @shost:	scsi host pointer that contains all scsi device siblings.
 *
 * scsi lld's with a FC transport call this routine to temporarily stop all
 * scsi commands to all devices managed by this host.  Called 
 * from interrupt or normal process context.
 *
 * Returns zero if successful or error if not
 *
 * Notes:
 *	The timeout and timer types are extracted from the fc transport 
 *	attributes from the caller's host pointer.  This routine assumes no
 *	locks are held on entry.
 **/
int
fc_host_block(struct Scsi_Host *shost)
{
	struct scsi_device *sdev;
	int timeout = fc_host_link_down_tmo(shost);
	struct work_struct *work = &fc_host_link_down_work(shost);

	if (timeout < 0 || timeout > SCSI_DEVICE_BLOCK_MAX_TIMEOUT)
		return -EINVAL;

	shost_for_each_device(sdev, shost) {
		scsi_internal_device_block(sdev);
	}

	schedule_delayed_work(work, timeout * HZ);

	return 0;
}
EXPORT_SYMBOL(fc_host_block);

/**
 * fc_host_unblock - unblock all devices managed by this host following a 
 *		fc_host_block request.
 * @shost:	scsi host containing all scsi device siblings to unblock.
 *
 * scsi lld's with a FC transport call this routine to restart IO to all scsi
 * devices managed by the specified scsi host following an fc_host_block 
 * request.  Called from interrupt or normal process context.
 *
 * Notes:       
 *	This routine assumes no locks are held on entry.
 **/
void
fc_host_unblock(struct Scsi_Host *shost)
{
	struct scsi_device *sdev;

	/* 
	 * Stop the host timer first. Take no action on the del_timer
	 * failure as the state machine state change will validate the
	 * transaction.
	 */
	if (cancel_delayed_work(&fc_host_link_down_work(shost)))
		flush_scheduled_work();

	shost_for_each_device(sdev, shost) {
		scsi_internal_device_unblock(sdev);
	}
}
EXPORT_SYMBOL(fc_host_unblock);

MODULE_AUTHOR("Martin Hicks");
MODULE_DESCRIPTION("FC Transport Attributes");
MODULE_LICENSE("GPL");

module_init(fc_transport_init);
module_exit(fc_transport_exit);
