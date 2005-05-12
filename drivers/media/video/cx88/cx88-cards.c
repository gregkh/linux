/*
 * $Id: cx88-cards.c,v 1.47 2004/11/03 09:04:50 kraxel Exp $
 *
 * device driver for Conexant 2388x based TV cards
 * card-specific stuff.
 *
 * (c) 2003 Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]
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
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>

#if defined(CONFIG_VIDEO_CX88_DVB) || defined(CONFIG_VIDEO_CX88_DVB_MODULE)
# define WITH_DVB 1
#endif

#include "cx88.h"
#ifdef WITH_DVB
#include "cx22702.h"
#endif

/* ------------------------------------------------------------------ */
/* board config info                                                  */

struct cx88_board cx88_boards[] = {
	[CX88_BOARD_UNKNOWN] = {
		.name		= "UNKNOWN/GENERIC",
		.tuner_type     = UNSET,
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 0,
		},{
			.type   = CX88_VMUX_COMPOSITE2,
			.vmux   = 1,
		},{
			.type   = CX88_VMUX_COMPOSITE3,
			.vmux   = 2,
		},{
			.type   = CX88_VMUX_COMPOSITE4,
			.vmux   = 3,
		}},
	},
	[CX88_BOARD_HAUPPAUGE] = {
		.name		= "Hauppauge WinTV 34xxx models",
		.tuner_type     = UNSET,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0xff00,  // internal decoder
		},{
			.type   = CX88_VMUX_DEBUG,
			.vmux   = 0,
			.gpio0  = 0xff01,  // mono from tuner chip
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0xff02,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0xff02,
		}},
		.radio = {
			.type   = CX88_RADIO,
			.gpio0  = 0xff01,
		},
	},
	[CX88_BOARD_GDI] = {
		.name		= "GDI Black Gold",
		.tuner_type     = UNSET,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
		}},
	},
	[CX88_BOARD_PIXELVIEW] = {
		.name           = "PixelView",
		.tuner_type     = UNSET,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0xff00,  // internal decoder
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
		}},
		.radio = {
			 .type  = CX88_RADIO,
			 .gpio0 = 0xff10,
		 },
	},
	[CX88_BOARD_ATI_WONDER_PRO] = {
		.name           = "ATI TV Wonder Pro",
		.tuner_type     = 44,
		.tda9887_conf   = TDA9887_PRESENT | TDA9887_INTERCARRIER,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
                        .gpio0  = 0x03ff,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
                        .gpio0  = 0x03fe,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
                        .gpio0  = 0x03fe,
		}},
	},
        [CX88_BOARD_WINFAST2000XP] = {
                .name           = "Leadtek Winfast 2000XP Expert",
                .tuner_type     = 44,
		.tda9887_conf   = TDA9887_PRESENT,
                .input          = {{
                        .type   = CX88_VMUX_TELEVISION,
                        .vmux   = 0,
			.gpio0	= 0x00F5e700,
			.gpio1  = 0x00003004,
			.gpio2  = 0x00F5e700,
			.gpio3  = 0x02000000,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0	= 0x00F5c700,
			.gpio1  = 0x00003004,
			.gpio2  = 0x00F5c700,
			.gpio3  = 0x02000000,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0	= 0x00F5c700,
			.gpio1  = 0x00003004,
			.gpio2  = 0x00F5c700,
			.gpio3  = 0x02000000,
                }},
                .radio = {
                        .type   = CX88_RADIO,
			.gpio0	= 0x00F5d700,
			.gpio1  = 0x00003004,
			.gpio2  = 0x00F5d700,
			.gpio3  = 0x02000000,
                },
        },
	[CX88_BOARD_AVERTV_303] = {
		.name           = "AverTV Studio 303 (M126)",
		.tuner_type     = 38,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio1  = 0x309f,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio1  = 0x305f,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio1  = 0x305f,
		}},
		.radio = {
			.type   = CX88_RADIO,
		},
	},
	[CX88_BOARD_MSI_TVANYWHERE_MASTER] = {
		// added gpio values thanks to Michal
		// values for PAL from DScaler
		.name           = "MSI TV-@nywhere Master",
		.tuner_type     = 33,
		.tda9887_conf	= TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x000040bf,
			.gpio1  = 0x000080c0,
			.gpio2  = 0x0000ff40,
		},{
                        .type   = CX88_VMUX_COMPOSITE1,
                        .vmux   = 1,
			.gpio0  = 0x000040bf,
			.gpio1  = 0x000080c0,
			.gpio2  = 0x0000ff40,
		},{
                        .type   = CX88_VMUX_SVIDEO,
                        .vmux   = 2,
			.gpio0  = 0x000040bf,
			.gpio1  = 0x000080c0,
			.gpio2  = 0x0000ff40,
                }},
                .radio = {
			 .type   = CX88_RADIO,
                },
	},
	[CX88_BOARD_WINFAST_DV2000] = {
                .name           = "Leadtek Winfast DV2000",
                .tuner_type     = 38,
		.tda9887_conf   = TDA9887_PRESENT,
                .input          = {{
                        .type   = CX88_VMUX_TELEVISION,
                        .vmux   = 0,
                }},
                .radio = {
                        .type   = CX88_RADIO,
                },
        },
        [CX88_BOARD_LEADTEK_PVR2000] = {
                .name           = "Leadtek PVR 2000",
                .tuner_type     = 38,
                .input          = {{
                        .type   = CX88_VMUX_TELEVISION,
                        .vmux   = 0,
                },{
                        .type   = CX88_VMUX_COMPOSITE1,
                        .vmux   = 1,
                },{
                        .type   = CX88_VMUX_SVIDEO,
                        .vmux   = 2,
                }},
                .radio = {
                        .type   = CX88_RADIO,
                },
		.blackbird = 1,
        },
	[CX88_BOARD_IODATA_GVVCP3PCI] = {
 		.name		= "IODATA GV-VCP3/PCI",
		.tuner_type     = TUNER_ABSENT,
 		.input          = {{
 			.type   = CX88_VMUX_COMPOSITE1,
 			.vmux   = 0,
 		},{
 			.type   = CX88_VMUX_COMPOSITE2,
 			.vmux   = 1,
 		},{
 			.type   = CX88_VMUX_SVIDEO,
 			.vmux   = 2,
 		}},
 	},
	[CX88_BOARD_PROLINK_PLAYTVPVR] = {
                .name           = "Prolink PlayTV PVR",
                .tuner_type     = 43,
		.tda9887_conf	= TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0xff00,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0xff03,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0xff03,
		}},
		.radio = {
			.type   = CX88_RADIO,
			.gpio0  = 0xff00,
		},
	},
	[CX88_BOARD_ASUS_PVR_416] = {
		.name		= "ASUS PVR-416",
		.tuner_type     = 43,
                .tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x0000fde6,
 		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x0000fde6, // 0x0000fda6 L,R RCA audio in?
		}},
                .radio = {
                        .type   = CX88_RADIO,
			.gpio0  = 0x0000fde2,
                },
		.blackbird = 1,
	},
	[CX88_BOARD_MSI_TVANYWHERE] = {
		.name           = "MSI TV-@nywhere",
		.tuner_type     = 33,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x00000fbf,
			.gpio2  = 0x0000fc08,
		},{
  			.type   = CX88_VMUX_COMPOSITE1,
  			.vmux   = 1,
			.gpio0  = 0x00000fbf,
			.gpio2  = 0x0000fc68,
		},{
  			.type   = CX88_VMUX_SVIDEO,
  			.vmux   = 2,
			.gpio0  = 0x00000fbf,
			.gpio2  = 0x0000fc68,
  		}},
	},
        [CX88_BOARD_KWORLD_DVB_T] = {
                .name           = "KWorld/VStream XPert DVB-T",
		.tuner_type     = TUNER_ABSENT,
                .input          = {{
                        .type   = CX88_VMUX_DVB,
                        .vmux   = 0,
                },{
                        .type   = CX88_VMUX_COMPOSITE1,
                        .vmux   = 1,
                },{
                        .type   = CX88_VMUX_SVIDEO,
                        .vmux   = 2,
                }},
		.dvb            = 1,
	},
	[CX88_BOARD_DVICO_FUSIONHDTV_DVB_T1] = {
		.name           = "DVICO FusionHDTV DVB-T1",
		.tuner_type     = TUNER_ABSENT, /* No analog tuner */
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x000027df,
		 },{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x000027df,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_KWORLD_LTV883] = {
		.name           = "KWorld LTV883RF",
                .tuner_type     = 48,
                .input          = {{
                        .type   = CX88_VMUX_TELEVISION,
                        .vmux   = 0,
                        .gpio0  = 0x07f8,
		},{
			.type   = CX88_VMUX_DEBUG,
			.vmux   = 0,
			.gpio0  = 0x07f9,  // mono from tuner chip
                },{
                        .type   = CX88_VMUX_COMPOSITE1,
                        .vmux   = 1,
                        .gpio0  = 0x000007fa,
                },{
                        .type   = CX88_VMUX_SVIDEO,
                        .vmux   = 2,
                        .gpio0  = 0x000007fa,
                }},
                .radio = {
                        .type   = CX88_RADIO,
                        .gpio0  = 0x000007f8,
                },
	},
	[CX88_BOARD_DVICO_FUSIONHDTV_3_GOLD] = {
		.name		= "DViCO - FusionHDTV 3 Gold",
		.tuner_type     = TUNER_MICROTUNE_4042FI5,
		/*
		   GPIO[0] resets DT3302 DTV receiver
		    0 - reset asserted
		    1 - normal operation
		   GPIO[1] mutes analog audio output connector
		    0 - enable selected source
		    1 - mute
		   GPIO[2] selects source for analog audio output connector
		    0 - analog audio input connector on tab
		    1 - analog DAC output from CX23881 chip
		   GPIO[3] selects RF input connector on tuner module
		    0 - RF connector labeled CABLE
		    1 - RF connector labeled ANT
		*/
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0	= 0x0f0d,
		},{
			.type   = CX88_VMUX_CABLE,
			.vmux   = 0,
			.gpio0	= 0x0f05,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0	= 0x0f00,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0	= 0x0f00,
		}},
#if 0
		.ts             = {
			 .type   = CX88_TS,
			 .gpio0  = 0x00000f01,   /* Hooked to tuner reset bit */
		 }
#endif
	},
        [CX88_BOARD_HAUPPAUGE_DVB_T1] = {
                .name           = "Hauppauge Nova-T DVB-T",
		.tuner_type     = TUNER_ABSENT,
                .input          = {{
                        .type   = CX88_VMUX_DVB,
                        .vmux   = 0,
                }},
		.dvb            = 1,
	},
        [CX88_BOARD_CONEXANT_DVB_T1] = {
		.name           = "Conexant DVB-T reference design",
		.tuner_type     = TUNER_ABSENT,
                .input          = {{
                        .type   = CX88_VMUX_DVB,
                        .vmux   = 0,
                }},
		.dvb            = 1,
	},
	[CX88_BOARD_PROVIDEO_PV259] = {
		.name		= "Provideo PV259",
		.tuner_type     = TUNER_PHILIPS_FQ1216ME,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
		}},
		.blackbird = 1,
	},
	[CX88_BOARD_DVICO_FUSIONHDTV_DVB_T_PLUS] = {
		.name           = "DVICO FusionHDTV DVB-T Plus",
		.tuner_type     = TUNER_ABSENT, /* No analog tuner */
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x000027df,
		 },{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x000027df,
		}},
		.dvb            = 1,
	},
};
const unsigned int cx88_bcount = ARRAY_SIZE(cx88_boards);

/* ------------------------------------------------------------------ */
/* PCI subsystem IDs                                                  */

struct cx88_subid cx88_subids[] = {
	{
		.subvendor = 0x0070,
		.subdevice = 0x3400,
		.card      = CX88_BOARD_HAUPPAUGE,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x3401,
		.card      = CX88_BOARD_HAUPPAUGE,
	},{
		.subvendor = 0x14c7,
		.subdevice = 0x0106,
		.card      = CX88_BOARD_GDI,
	},{
		.subvendor = 0x14c7,
		.subdevice = 0x0107, /* with mpeg encoder */
		.card      = CX88_BOARD_GDI,
	},{
		.subvendor = PCI_VENDOR_ID_ATI,
		.subdevice = 0x00f8,
		.card      = CX88_BOARD_ATI_WONDER_PRO,
	},{
                .subvendor = 0x107d,
                .subdevice = 0x6611,
                .card      = CX88_BOARD_WINFAST2000XP,
	},{
                .subvendor = 0x107d,
                .subdevice = 0x6613,	/* NTSC */
                .card      = CX88_BOARD_WINFAST2000XP,
	},{
		.subvendor = 0x107d,
                .subdevice = 0x6620,
                .card      = CX88_BOARD_WINFAST_DV2000,
        },{
                .subvendor = 0x107d,
                .subdevice = 0x663b,
                .card      = CX88_BOARD_LEADTEK_PVR2000,
        },{
                .subvendor = 0x107d,
                .subdevice = 0x663C,
                .card      = CX88_BOARD_LEADTEK_PVR2000,
        },{
		.subvendor = 0x1461,
		.subdevice = 0x000b,
		.card      = CX88_BOARD_AVERTV_303,
	},{
		.subvendor = 0x1462,
		.subdevice = 0x8606,
		.card      = CX88_BOARD_MSI_TVANYWHERE_MASTER,
	},{
 		.subvendor = 0x10fc,
 		.subdevice = 0xd003,
 		.card      = CX88_BOARD_IODATA_GVVCP3PCI,
	},{
 		.subvendor = 0x1043,
 		.subdevice = 0x4823,  /* with mpeg encoder */
 		.card      = CX88_BOARD_ASUS_PVR_416,
	},{
		.subvendor = 0x17de,
		.subdevice = 0x08a6,
		.card      = CX88_BOARD_KWORLD_DVB_T,
	},{
		.subvendor = 0x18ac,
		.subdevice = 0xd810,
		.card      = CX88_BOARD_DVICO_FUSIONHDTV_3_GOLD,
	},{
		.subvendor = 0x18AC,
		.subdevice = 0xDB00,
		.card      = CX88_BOARD_DVICO_FUSIONHDTV_DVB_T1,
 	},{
		.subvendor = 0x0070,
		.subdevice = 0x9002,
		.card      = CX88_BOARD_HAUPPAUGE_DVB_T1,
 	},{
		.subvendor = 0x14f1,
		.subdevice = 0x0187,
		.card      = CX88_BOARD_CONEXANT_DVB_T1,
 	},{
		.subvendor = 0x1540,
		.subdevice = 0x2580,
		.card      = CX88_BOARD_PROVIDEO_PV259,
	},{
		.subvendor = 0x18AC,
		.subdevice = 0xDB10,
		.card      = CX88_BOARD_DVICO_FUSIONHDTV_DVB_T_PLUS,
	}
};
const unsigned int cx88_idcount = ARRAY_SIZE(cx88_subids);

/* ----------------------------------------------------------------------- */
/* some leadtek specific stuff                                             */

static void __devinit leadtek_eeprom(struct cx88_core *core, u8 *eeprom_data)
{
	/* This is just for the Winfast 2000 XP board ATM; I don't have data on
	 * any others.
	 *
	 * Byte 0 is 1 on the NTSC board.
	 */

	if (eeprom_data[4] != 0x7d ||
	    eeprom_data[5] != 0x10 ||
	    eeprom_data[7] != 0x66) {
		printk(KERN_WARNING "%s: Leadtek eeprom invalid.\n",
		       core->name);
		return;
	}

	core->has_radio  = 1;
	core->tuner_type = (eeprom_data[6] == 0x13) ? 43 : 38;

	printk(KERN_INFO "%s: Leadtek Winfast 2000 XP config: "
	       "tuner=%d, eeprom[0]=0x%02x\n",
	       core->name, core->tuner_type, eeprom_data[0]);
}


/* ----------------------------------------------------------------------- */
/* some hauppauge specific stuff                                           */

static struct {
        int  id;
        char *name;
} hauppauge_tuner[] __devinitdata = {
        { TUNER_ABSENT,        "" },
        { TUNER_ABSENT,        "External" },
        { TUNER_ABSENT,        "Unspecified" },
        { TUNER_PHILIPS_PAL,   "Philips FI1216" },
        { TUNER_PHILIPS_SECAM, "Philips FI1216MF" },
        { TUNER_PHILIPS_NTSC,  "Philips FI1236" },
        { TUNER_PHILIPS_PAL_I, "Philips FI1246" },
        { TUNER_PHILIPS_PAL_DK,"Philips FI1256" },
        { TUNER_PHILIPS_PAL,   "Philips FI1216 MK2" },
        { TUNER_PHILIPS_SECAM, "Philips FI1216MF MK2" },
        { TUNER_PHILIPS_NTSC,  "Philips FI1236 MK2" },
        { TUNER_PHILIPS_PAL_I, "Philips FI1246 MK2" },
        { TUNER_PHILIPS_PAL_DK,"Philips FI1256 MK2" },
        { TUNER_TEMIC_NTSC,    "Temic 4032FY5" },
        { TUNER_TEMIC_PAL,     "Temic 4002FH5" },
        { TUNER_TEMIC_PAL_I,   "Temic 4062FY5" },
        { TUNER_PHILIPS_PAL,   "Philips FR1216 MK2" },
        { TUNER_PHILIPS_SECAM, "Philips FR1216MF MK2" },
        { TUNER_PHILIPS_NTSC,  "Philips FR1236 MK2" },
        { TUNER_PHILIPS_PAL_I, "Philips FR1246 MK2" },
        { TUNER_PHILIPS_PAL_DK,"Philips FR1256 MK2" },
        { TUNER_PHILIPS_PAL,   "Philips FM1216" },
        { TUNER_PHILIPS_SECAM, "Philips FM1216MF" },
        { TUNER_PHILIPS_NTSC,  "Philips FM1236" },
        { TUNER_PHILIPS_PAL_I, "Philips FM1246" },
        { TUNER_PHILIPS_PAL_DK,"Philips FM1256" },
        { TUNER_TEMIC_4036FY5_NTSC, "Temic 4036FY5" },
        { TUNER_ABSENT,        "Samsung TCPN9082D" },
        { TUNER_ABSENT,        "Samsung TCPM9092P" },
        { TUNER_TEMIC_4006FH5_PAL, "Temic 4006FH5" },
        { TUNER_ABSENT,        "Samsung TCPN9085D" },
        { TUNER_ABSENT,        "Samsung TCPB9085P" },
        { TUNER_ABSENT,        "Samsung TCPL9091P" },
        { TUNER_TEMIC_4039FR5_NTSC, "Temic 4039FR5" },
        { TUNER_PHILIPS_FQ1216ME,   "Philips FQ1216 ME" },
        { TUNER_TEMIC_4066FY5_PAL_I, "Temic 4066FY5" },
        { TUNER_PHILIPS_NTSC,        "Philips TD1536" },
        { TUNER_PHILIPS_NTSC,        "Philips TD1536D" },
	{ TUNER_PHILIPS_NTSC,  "Philips FMR1236" }, /* mono radio */
        { TUNER_ABSENT,        "Philips FI1256MP" },
        { TUNER_ABSENT,        "Samsung TCPQ9091P" },
        { TUNER_TEMIC_4006FN5_MULTI_PAL, "Temic 4006FN5" },
        { TUNER_TEMIC_4009FR5_PAL, "Temic 4009FR5" },
        { TUNER_TEMIC_4046FM5,     "Temic 4046FM5" },
	{ TUNER_TEMIC_4009FN5_MULTI_PAL_FM, "Temic 4009FN5" },
	{ TUNER_ABSENT,        "Philips TD1536D_FH_44"},
	{ TUNER_LG_NTSC_FM,    "LG TPI8NSR01F"},
	{ TUNER_LG_PAL_FM,     "LG TPI8PSB01D"},
	{ TUNER_LG_PAL,        "LG TPI8PSB11D"},
	{ TUNER_LG_PAL_I_FM,   "LG TAPC-I001D"},
	{ TUNER_LG_PAL_I,      "LG TAPC-I701D"}
};

static void hauppauge_eeprom(struct cx88_core *core, u8 *eeprom_data)
{
	unsigned int blk2,tuner,radio,model;

	if (eeprom_data[0] != 0x84 || eeprom_data[2] != 0) {
		printk(KERN_WARNING "%s: Hauppauge eeprom: invalid\n",
		       core->name);
		return;
	}

	/* Block 2 starts after len+3 bytes header */
	blk2 = eeprom_data[1] + 3;

	/* decode + use some config infos */
	model = eeprom_data[12] << 8 | eeprom_data[11];
	tuner = eeprom_data[9];
	radio = eeprom_data[blk2-1] & 0x01;

        if (tuner < ARRAY_SIZE(hauppauge_tuner))
                core->tuner_type = hauppauge_tuner[tuner].id;
	if (radio)
		core->has_radio = 1;

	printk(KERN_INFO "%s: hauppauge eeprom: model=%d, "
	       "tuner=%s (%d), radio=%s\n",
	       core->name, model, (tuner < ARRAY_SIZE(hauppauge_tuner)
				   ? hauppauge_tuner[tuner].name : "?"),
	       core->tuner_type, radio ? "yes" : "no");
}

#ifdef WITH_DVB
static int hauppauge_eeprom_dvb(struct cx88_core *core, u8 *ee)
{
	int model;
	int tuner;
	char *tname;

	/* Make sure we support the board model */
	model = ee[0x1f] << 24 | ee[0x1e] << 16 | ee[0x1d] << 8 | ee[0x1c];
	switch(model) {
	case 90002:
	case 90500:
	case 90501:
		/* known */
		break;
	default:
		printk("%s: warning: unknown hauppauge model #%d\n",
		       core->name, model);
		break;
	}

	/* Make sure we support the tuner */
	tuner = ee[0x2d];
	switch(tuner) {
	case 0x4B:
		tname = "Thomson DTT 7595";
		core->pll_type = PLLTYPE_DTT7595;
		break;
	case 0x4C:
		tname = "Thomson DTT 7592";
		core->pll_type = PLLTYPE_DTT7592;
		break;
	default:
		printk("%s: error: unknown hauppauge tuner 0x%02x\n",
		       core->name, tuner);
		return -ENODEV;
	}
	printk(KERN_INFO "%s: hauppauge eeprom: model=%d, tuner=%s (%d)\n",
	       core->name, model, tname, tuner);

	core->pll_addr = 0x61;
	core->demod_addr = 0x43;
}
#endif

/* ----------------------------------------------------------------------- */
/* some GDI (was: Modular Technology) specific stuff                       */

static struct {
	int  id;
	int  fm;
	char *name;
} gdi_tuner[] = {
	[ 0x01 ] = { .id   = TUNER_ABSENT,
		     .name = "NTSC_M" },
	[ 0x02 ] = { .id   = TUNER_ABSENT,
		     .name = "PAL_B" },
	[ 0x03 ] = { .id   = TUNER_ABSENT,
		     .name = "PAL_I" },
	[ 0x04 ] = { .id   = TUNER_ABSENT,
		     .name = "PAL_D" },
	[ 0x05 ] = { .id   = TUNER_ABSENT,
		     .name = "SECAM" },

	[ 0x10 ] = { .id   = TUNER_ABSENT,
		     .fm   = 1,
		     .name = "TEMIC_4049" },
	[ 0x11 ] = { .id   = TUNER_TEMIC_4136FY5,
		     .name = "TEMIC_4136" },
	[ 0x12 ] = { .id   = TUNER_ABSENT,
		     .name = "TEMIC_4146" },

	[ 0x20 ] = { .id   = TUNER_PHILIPS_FQ1216ME,
		     .fm   = 1,
		     .name = "PHILIPS_FQ1216_MK3" },
	[ 0x21 ] = { .id   = TUNER_ABSENT, .fm = 1,
		     .name = "PHILIPS_FQ1236_MK3" },
	[ 0x22 ] = { .id   = TUNER_ABSENT,
		     .name = "PHILIPS_FI1236_MK3" },
	[ 0x23 ] = { .id   = TUNER_ABSENT,
		     .name = "PHILIPS_FI1216_MK3" },
};

static void gdi_eeprom(struct cx88_core *core, u8 *eeprom_data)
{
	char *name = (eeprom_data[0x0d] < ARRAY_SIZE(gdi_tuner))
		? gdi_tuner[eeprom_data[0x0d]].name : NULL;

	printk(KERN_INFO "%s: GDI: tuner=%s\n", core->name,
	       name ? name : "unknown");
	if (NULL == name)
		return;
	core->tuner_type = gdi_tuner[eeprom_data[0x0d]].id;
	core->has_radio  = gdi_tuner[eeprom_data[0x0d]].fm;
}

/* ----------------------------------------------------------------------- */

static int
i2c_eeprom(struct i2c_client *c, unsigned char *eedata, int len)
{
	unsigned char buf;
	int err;

	c->addr = 0xa0 >> 1;
	buf = 0;
	if (1 != (err = i2c_master_send(c,&buf,1))) {
		printk(KERN_INFO "cx88: Huh, no eeprom present (err=%d)?\n",
		       err);
		return -1;
	}
	if (len != (err = i2c_master_recv(c,eedata,len))) {
		printk(KERN_WARNING "cx88: i2c eeprom read error (err=%d)\n",
		       err);
		return -1;
	}
#if 0
	for (i = 0; i < len; i++) {
		if (0 == (i % 16))
			printk(KERN_INFO "cx88 ee: %02x:",i);
		printk(" %02x",eedata[i]);
		if (15 == (i % 16))
			printk("\n");
	}
#endif
	return 0;
}

void cx88_card_list(struct cx88_core *core, struct pci_dev *pci)
{
	int i;

	if (0 == pci->subsystem_vendor &&
	    0 == pci->subsystem_device) {
		printk("%s: Your board has no valid PCI Subsystem ID and thus can't\n"
		       "%s: be autodetected.  Please pass card=<n> insmod option to\n"
		       "%s: workaround that.  Redirect complaints to the vendor of\n"
		       "%s: the TV card.  Best regards,\n"
		       "%s:         -- tux\n",
		       core->name,core->name,core->name,core->name,core->name);
	} else {
		printk("%s: Your board isn't known (yet) to the driver.  You can\n"
		       "%s: try to pick one of the existing card configs via\n"
		       "%s: card=<n> insmod option.  Updating to the latest\n"
		       "%s: version might help as well.\n",
		       core->name,core->name,core->name,core->name);
	}
	printk("%s: Here is a list of valid choices for the card=<n> insmod option:\n",
	       core->name);
	for (i = 0; i < cx88_bcount; i++)
		printk("%s:    card=%d -> %s\n",
		       core->name, i, cx88_boards[i].name);
}

void cx88_card_setup(struct cx88_core *core)
{
	static u8 eeprom[128];

	switch (core->board) {
	case CX88_BOARD_HAUPPAUGE:
		if (0 == core->i2c_rc)
			i2c_eeprom(&core->i2c_client,eeprom,sizeof(eeprom));
		hauppauge_eeprom(core,eeprom+8);
		break;
	case CX88_BOARD_GDI:
		if (0 == core->i2c_rc)
			i2c_eeprom(&core->i2c_client,eeprom,sizeof(eeprom));
		gdi_eeprom(core,eeprom);
		break;
	case CX88_BOARD_WINFAST2000XP:
		if (0 == core->i2c_rc)
			i2c_eeprom(&core->i2c_client,eeprom,sizeof(eeprom));
		leadtek_eeprom(core,eeprom);
		break;
	case CX88_BOARD_DVICO_FUSIONHDTV_DVB_T1:
		/* Tuner reset is hooked to  the tuner out of reset */
		cx_set(MO_GP0_IO, 0x00000101);
		cx_clear(MO_GP0_IO, 0x00000001);
		msleep(1);
		cx_set(MO_GP0_IO, 0x00000101);
		break;
#ifdef WITH_DVB
	case CX88_BOARD_HAUPPAUGE_DVB_T1:
		if (0 == core->i2c_rc)
			i2c_eeprom(&core->i2c_client,eeprom,sizeof(eeprom));
		hauppauge_eeprom_dvb(core,eeprom);
		break;
	case CX88_BOARD_CONEXANT_DVB_T1:
		core->pll_type   = PLLTYPE_DTT7579;
		core->pll_addr   = 0x60;
		core->demod_addr = 0x43;
		break;
#endif
	}
	if (cx88_boards[core->board].radio.type == CX88_RADIO)
		core->has_radio = 1;
}

/* ------------------------------------------------------------------ */

EXPORT_SYMBOL(cx88_boards);
EXPORT_SYMBOL(cx88_bcount);
EXPORT_SYMBOL(cx88_subids);
EXPORT_SYMBOL(cx88_idcount);
EXPORT_SYMBOL(cx88_card_list);
EXPORT_SYMBOL(cx88_card_setup);

/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
