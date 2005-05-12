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
#ifndef SCSI_TRANSPORT_FC_H
#define SCSI_TRANSPORT_FC_H

#include <linux/config.h>

struct scsi_transport_template;


/*
 * FC Port definitions - Following FC HBAAPI guidelines
 *
 * Note: Not all binary values for the different fields match HBAAPI.
 *  Instead, we use densely packed ordinal values or enums.
 *  We get away with this as we never present the actual binary values
 *  externally. For sysfs, we always present the string that describes
 *  the value. Thus, an admin doesn't need a magic HBAAPI decoder ring
 *  to understand the values. The HBAAPI user-space library is free to
 *  convert the strings into the HBAAPI-specified binary values.
 *
 * Note: Not all HBAAPI-defined values are contained in the definitions
 *  below. Those not appropriate to an fc_host (e.g. FCP initiator) have
 *  been removed.
 */

/*
 * fc_port_type: If you alter this, you also need to alter scsi_transport_fc.c
 * (for the ascii descriptions).
 */
enum fc_port_type {
	FC_PORTTYPE_UNKNOWN,
	FC_PORTTYPE_OTHER,
	FC_PORTTYPE_NOTPRESENT,
	FC_PORTTYPE_NPORT,		/* Attached to FPort */
	FC_PORTTYPE_NLPORT,		/* (Public) Loop w/ FLPort */
	FC_PORTTYPE_LPORT,		/* (Private) Loop w/o FLPort */
	FC_PORTTYPE_PTP,		/* Point to Point w/ another NPort */
};

/*
 * fc_port_state: If you alter this, you also need to alter scsi_transport_fc.c
 * (for the ascii descriptions).
 */
enum fc_port_state {
	FC_PORTSTATE_UNKNOWN,
	FC_PORTSTATE_ONLINE,
	FC_PORTSTATE_OFFLINE,		/* User has taken Port Offline */
	FC_PORTSTATE_BYPASSED,
	FC_PORTSTATE_DIAGNOSTICS,
	FC_PORTSTATE_LINKDOWN,
	FC_PORTSTATE_ERROR,
	FC_PORTSTATE_LOOPBACK,
};


/* 
 * FC Classes of Service
 * Note: values are not enumerated, as they can be "or'd" together
 * for reporting (e.g. report supported_classes). If you alter this list,
 * you also need to alter scsi_transport_fc.c (for the ascii descriptions).
 */
#define FC_COS_UNSPECIFIED		0
#define FC_COS_CLASS1			2
#define FC_COS_CLASS2			4
#define FC_COS_CLASS3			8
#define FC_COS_CLASS4			0x10
#define FC_COS_CLASS6			0x40

/* 
 * FC Port Speeds
 * Note: values are not enumerated, as they can be "or'd" together
 * for reporting (e.g. report supported_speeds). If you alter this list,
 * you also need to alter scsi_transport_fc.c (for the ascii descriptions).
 */
#define FC_PORTSPEED_UNKNOWN		0 /* Unknown - transceiver
					     incapable of reporting */
#define FC_PORTSPEED_1GBIT		1
#define FC_PORTSPEED_2GBIT		2
#define FC_PORTSPEED_10GBIT		4
#define FC_PORTSPEED_4GBIT		8
#define FC_PORTSPEED_NOT_NEGOTIATED	(1 << 15) /* Speed not established */

/*
 * fc_tgtid_binding_type: If you alter this, you also need to alter
 * scsi_transport_fc.c (for the ascii descriptions).
 */
enum fc_tgtid_binding_type  {
	FC_TGTID_BIND_BY_WWPN,
	FC_TGTID_BIND_BY_WWNN,
	FC_TGTID_BIND_BY_ID,
};



/*
 * FC Remote Port (Target) Attributes
 */

struct fc_starget_attrs {	/* aka fc_target_attrs */
	int port_id;
	u64 node_name;
	u64 port_name;
	u32 dev_loss_tmo;	/* Remote Port loss timeout in seconds. */
	struct work_struct dev_loss_work;
};

#define fc_starget_port_id(x) \
	(((struct fc_starget_attrs *)&(x)->starget_data)->port_id)
#define fc_starget_node_name(x) \
	(((struct fc_starget_attrs *)&(x)->starget_data)->node_name)
#define fc_starget_port_name(x)	\
	(((struct fc_starget_attrs *)&(x)->starget_data)->port_name)
#define fc_starget_dev_loss_tmo(x) \
	(((struct fc_starget_attrs *)&(x)->starget_data)->dev_loss_tmo)
#define fc_starget_dev_loss_work(x) \
	(((struct fc_starget_attrs *)&(x)->starget_data)->dev_loss_work)


/*
 * FC Local Port (Host) Statistics
 */

/* FC Statistics - Following FC HBAAPI v2.0 guidelines */
struct fc_host_statistics {
	/* port statistics */
	u64 seconds_since_last_reset;
	u64 tx_frames;
	u64 tx_words;
	u64 rx_frames;
	u64 rx_words;
	u64 lip_count;
	u64 nos_count;
	u64 error_frames;
	u64 dumped_frames;
	u64 link_failure_count;
	u64 loss_of_sync_count;
	u64 loss_of_signal_count;
	u64 prim_seq_protocol_err_count;
	u64 invalid_tx_word_count;
	u64 invalid_crc_count;
	
	/* fc4 statistics  (only FCP supported currently) */
	u64 fcp_input_requests;
	u64 fcp_output_requests;
	u64 fcp_control_requests;
	u64 fcp_input_megabytes;
	u64 fcp_output_megabytes;
};


/*
 * FC Local Port (Host) Attributes
 *
 * Attributes are based on HBAAPI V2.0 definitions.
 * Note: OSDeviceName is determined by user-space library
 *
 * Fixed attributes are not expected to change. The driver is
 * expected to set these values after successfully calling scsi_add_host().
 * The transport fully manages all get functions w/o driver interaction.
 *
 * Dynamic attributes are expected to change. The driver participates
 * in all get/set operations via functions provided by the driver.
 *
 * Private attributes are transport-managed values. They are fully
 * managed by the transport w/o driver interaction.
 */

#define FC_FC4_LIST_SIZE		32
#define FC_SYMBOLIC_NAME_SIZE		256
#define FC_VERSION_STRING_SIZE		64
#define FC_SERIAL_NUMBER_SIZE		80

struct fc_host_attrs {
	/* Fixed Attributes */
	u64 node_name;
	u64 port_name;
	u32 supported_classes;
	u8  supported_fc4s[FC_FC4_LIST_SIZE];
	char symbolic_name[FC_SYMBOLIC_NAME_SIZE];
	u32 supported_speeds;
	u32 maxframe_size;
	char hardware_version[FC_VERSION_STRING_SIZE];
	char firmware_version[FC_VERSION_STRING_SIZE];
	char serial_number[FC_SERIAL_NUMBER_SIZE];
	char opt_rom_version[FC_VERSION_STRING_SIZE];
	char driver_version[FC_VERSION_STRING_SIZE];

	/* Dynamic Attributes */
	u32 port_id;
	enum fc_port_type port_type;
	enum fc_port_state port_state;
	u8  active_fc4s[FC_FC4_LIST_SIZE];
	u32 speed;
	u64 fabric_name;
	u32 link_down_tmo;	/* Link Down timeout in seconds. */

	/* Private (Transport-managed) Attributes */
	enum fc_tgtid_binding_type  tgtid_bind_type;

	/* internal data */
	struct work_struct link_down_work;
};

#define fc_host_node_name(x) \
	(((struct fc_host_attrs *)(x)->shost_data)->node_name)
#define fc_host_port_name(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->port_name)
#define fc_host_supported_classes(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->supported_classes)
#define fc_host_supported_fc4s(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->supported_fc4s)
#define fc_host_symbolic_name(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->symbolic_name)
#define fc_host_supported_speeds(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->supported_speeds)
#define fc_host_maxframe_size(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->maxframe_size)
#define fc_host_hardware_version(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->hardware_version)
#define fc_host_firmware_version(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->firmware_version)
#define fc_host_serial_number(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->serial_number)
#define fc_host_opt_rom_version(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->opt_rom_version)
#define fc_host_driver_version(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->driver_version)
#define fc_host_port_id(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->port_id)
#define fc_host_port_type(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->port_type)
#define fc_host_port_state(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->port_state)
#define fc_host_active_fc4s(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->active_fc4s)
#define fc_host_speed(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->speed)
#define fc_host_fabric_name(x)	\
	(((struct fc_host_attrs *)(x)->shost_data)->fabric_name)
#define fc_host_link_down_tmo(x) \
	(((struct fc_host_attrs *)(x)->shost_data)->link_down_tmo)
#define fc_host_tgtid_bind_type(x) \
	(((struct fc_host_attrs *)(x)->shost_data)->tgtid_bind_type)
#define fc_host_link_down_work(x) \
	(((struct fc_host_attrs *)(x)->shost_data)->link_down_work)


/* The functions by which the transport class and the driver communicate */
struct fc_function_template {
	void 	(*get_starget_port_id)(struct scsi_target *);
	void	(*get_starget_node_name)(struct scsi_target *);
	void	(*get_starget_port_name)(struct scsi_target *);
	void    (*get_starget_dev_loss_tmo)(struct scsi_target *);
	void	(*set_starget_dev_loss_tmo)(struct scsi_target *, u32);

	void 	(*get_host_port_id)(struct Scsi_Host *);
	void	(*get_host_port_type)(struct Scsi_Host *);
	void	(*get_host_port_state)(struct Scsi_Host *);
	void	(*get_host_active_fc4s)(struct Scsi_Host *);
	void	(*get_host_speed)(struct Scsi_Host *);
	void	(*get_host_fabric_name)(struct Scsi_Host *);
	void    (*get_host_link_down_tmo)(struct Scsi_Host *);
	void	(*set_host_link_down_tmo)(struct Scsi_Host *, u32);

	struct fc_host_statistics * (*get_fc_host_stats)(struct Scsi_Host *);
	void	(*reset_fc_host_stats)(struct Scsi_Host *);

	/* 
	 * The driver sets these to tell the transport class it
	 * wants the attributes displayed in sysfs.  If the show_ flag
	 * is not set, the attribute will be private to the transport
	 * class 
	 */
	unsigned long	show_starget_port_id:1;
	unsigned long	show_starget_node_name:1;
	unsigned long	show_starget_port_name:1;
	unsigned long   show_starget_dev_loss_tmo:1;

	/* host fixed attributes */
	unsigned long	show_host_node_name:1;
	unsigned long	show_host_port_name:1;
	unsigned long	show_host_supported_classes:1;
	unsigned long	show_host_supported_fc4s:1;
	unsigned long	show_host_symbolic_name:1;
	unsigned long	show_host_supported_speeds:1;
	unsigned long	show_host_maxframe_size:1;
	unsigned long	show_host_hardware_version:1;
	unsigned long	show_host_firmware_version:1;
	unsigned long	show_host_serial_number:1;
	unsigned long	show_host_opt_rom_version:1;
	unsigned long	show_host_driver_version:1;
	/* host dynamic attributes */
	unsigned long	show_host_port_id:1;
	unsigned long	show_host_port_type:1;
	unsigned long	show_host_port_state:1;
	unsigned long	show_host_active_fc4s:1;
	unsigned long	show_host_speed:1;
	unsigned long	show_host_fabric_name:1;
	unsigned long   show_host_link_down_tmo:1;
};


struct scsi_transport_template *fc_attach_transport(struct fc_function_template *);
void fc_release_transport(struct scsi_transport_template *);
int fc_target_block(struct scsi_target *starget);
void fc_target_unblock(struct scsi_target *starget);
int fc_host_block(struct Scsi_Host *shost);
void fc_host_unblock(struct Scsi_Host *shost);

#endif /* SCSI_TRANSPORT_FC_H */
