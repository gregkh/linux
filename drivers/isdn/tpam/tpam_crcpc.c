/* $Id: tpam_crcpc.c,v 1.1.2.2 2001/09/23 22:25:03 kai Exp $
 *
 * Turbo PAM ISDN driver for Linux. (Kernel Driver - CRC encoding)
 *
 * Copyright 1998-2000 AUVERTECH Télécom
 * Copyright 2001 Stelian Pop <stelian.pop@fr.alcove.com>, Alcôve
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 *
 * For all support questions please contact: <support@auvertech.fr>
 *
 */

/*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Module Name:

    crcpc.c

Abstract:

    Modem HDLC coding
    Software HDLC coding / decoding

Revision History:

---------------------------------------------------------------------------*/

#include <linux/crc-ccitt.h>
#include "tpam.h"

#define  HDLC_CTRL_CHAR_CMPL_MASK	0x20	/* HDLC control character complement mask */
#define  HDLC_FLAG			0x7E	/* HDLC flag */
#define  HDLC_CTRL_ESC			0x7D	/* HDLC control escapr character */
#define  HDLC_LIKE_FCS_INIT_VAL		0xFFFF	/* FCS initial value (0xFFFF for new equipment or 0) */
#define  HDLC_FCS_OK			0xF0B8	/* This value is the only valid value of FCS */

#define TRUE	1
#define FALSE	0

static u8 ap_t_ctrl_char_complemented[256]; /* list of characters to complement */

static void ap_hdlc_like_ctrl_char_list (u32 ctrl_char) {
	int i;

	for (i = 0; i < 256; ++i)
		ap_t_ctrl_char_complemented[i] = FALSE;
	for (i = 0; i < 32; ++i)
		if ((ctrl_char >> i) & 0x0001)
			ap_t_ctrl_char_complemented [i] = TRUE;
	ap_t_ctrl_char_complemented[HDLC_FLAG] = TRUE;
	ap_t_ctrl_char_complemented[HDLC_CTRL_ESC] = TRUE;
		
}

void init_CRC(void) {
	ap_hdlc_like_ctrl_char_list(0xffffffff);
}

void hdlc_encode_modem(u8 *buffer_in, u32 lng_in,
		       u8 *buffer_out, u32 *lng_out) {
	u16 fcs;
	register u8 data;
	register u8 *p_data_out = buffer_out;

	fcs = HDLC_LIKE_FCS_INIT_VAL;

	/*
	 *   Insert HDLC flag at the beginning of the frame
	 */
	*p_data_out++ = HDLC_FLAG;

#define ESCAPE_CHAR(data_out, data) \
	if (ap_t_ctrl_char_complemented[data]) { \
		*data_out++ = HDLC_CTRL_ESC; \
		*data_out++ = data ^ 0x20; \
	} \
	else \
		*data_out++ = data;

	while (lng_in--) {
		data = *buffer_in++;

		/*
		 *   FCS calculation
		 */
		fcs = crc_ccitt_byte(fcs, data);

		ESCAPE_CHAR(p_data_out, data);
	}

	/*
	 *  Add FCS and closing flag
	 */
	fcs ^= 0xFFFF;  // Complement

	data = (u8)(fcs & 0xff);	/* LSB */
	ESCAPE_CHAR(p_data_out, data);

	data = (u8)((fcs >> 8));	/* MSB */
	ESCAPE_CHAR(p_data_out, data);
#undef ESCAPE_CHAR

	*p_data_out++ = HDLC_FLAG;

	*lng_out = (u32)(p_data_out - buffer_out);
}

void hdlc_no_accm_encode(u8 *buffer_in, u32 lng_in, 
			 u8 *buffer_out, u32 *lng_out) {
	u16 fcs;
	register u8 data;
	register u8 *p_data_out = buffer_out;

	/*
	 *   Insert HDLC flag at the beginning of the frame
	 */
	fcs = HDLC_LIKE_FCS_INIT_VAL;

	while (lng_in--) {
		data = *buffer_in++;
		/* calculate FCS */
		fcs = crc_ccitt_byte(fcs, data);
		*p_data_out++ = data;
	}

	/*
	 *  Add FCS and closing flag
	 */
	fcs ^= 0xFFFF;  // Complement
	data = (u8)(fcs);    
	*p_data_out++ = data;

	data =(u8)((fcs >> 8));   // revense MSB / LSB
	*p_data_out++ = data;
 
	*lng_out = (u32)(p_data_out - buffer_out);
}

u32 hdlc_no_accm_decode(u8 *buffer_in, u32 lng_in) {
	u16 fcs;
	u32 lng = lng_in;
	register u8 data;

	/*
	 *   Insert HDLC flag at the beginning of the frame
	 */
	fcs = HDLC_LIKE_FCS_INIT_VAL;

	while (lng_in--) {
		data = *buffer_in++;
		/* calculate FCS */
		fcs = crc_ccitt_byte(fcs, data);
	}

	if (fcs == HDLC_FCS_OK) 
		return (lng-2);
	else 
		return 0;
}

