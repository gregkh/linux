/*
 * Access vector cache interface for the security server.
 *
 * Author : Stephen Smalley, <sds@epoch.ncsc.mil>
 */
#ifndef _SELINUX_AVC_SS_H_
#define _SELINUX_AVC_SS_H_

#include "flask.h"

int avc_ss_grant(u32 ssid, u32 tsid, u16 tclass, u32 perms, u32 seqno);

int avc_ss_try_revoke(u32 ssid, u32 tsid, u16 tclass, u32 perms, u32 seqno,
		      u32 *out_retained);

int avc_ss_revoke(u32 ssid, u32 tsid, u16 tclass, u32 perms, u32 seqno);

int avc_ss_reset(u32 seqno);

int avc_ss_set_auditallow(u32 ssid, u32 tsid, u16 tclass, u32 perms,
			  u32 seqno, u32 enable);

int avc_ss_set_auditdeny(u32 ssid, u32 tsid, u16 tclass, u32 perms,
			 u32 seqno, u32 enable);

#endif /* _SELINUX_AVC_SS_H_ */

