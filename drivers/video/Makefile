# Makefile for the Linux video drivers.
# 5 Aug 1999, James Simmons, <mailto:jsimmons@users.sf.net>
# Rewritten to use lists instead of if-statements.

# Each configuration option enables a list of files.

obj-$(CONFIG_VT)		  += console/
obj-$(CONFIG_LOGO)		  += logo/
obj-$(CONFIG_SYSFS)		  += backlight/

obj-$(CONFIG_FB)                  += fbmem.o fbmon.o fbcmap.o fbsysfs.o modedb.o softcursor.o
# Only include macmodes.o if we have FB support and are PPC
ifeq ($(CONFIG_FB),y)
obj-$(CONFIG_PPC)                 += macmodes.o
endif

# Hardware specific drivers go first
obj-$(CONFIG_FB_RETINAZ3)         += retz3fb.o
obj-$(CONFIG_FB_AMIGA)            += amifb.o c2p.o
obj-$(CONFIG_FB_CLPS711X)         += clps711xfb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_CYBER)            += cyberfb.o
obj-$(CONFIG_FB_CYBER2000)        += cyber2000fb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_PM2)              += pm2fb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_PM3)		  += pm3fb.o

obj-$(CONFIG_FB_MATROX)		  += matrox/ cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_RIVA)		  += riva/ cfbimgblt.o cfbfillrect.o \
				     cfbcopyarea.o vgastate.o
obj-$(CONFIG_FB_ATY)		  += aty/ cfbcopyarea.o cfbfillrect.o cfbimgblt.o
obj-$(CONFIG_FB_ATY128)		  += aty/ cfbcopyarea.o cfbfillrect.o cfbimgblt.o
obj-$(CONFIG_FB_RADEON)		  += aty/ cfbcopyarea.o cfbfillrect.o cfbimgblt.o
obj-$(CONFIG_FB_SIS)		  += sis/ cfbcopyarea.o cfbfillrect.o cfbimgblt.o
obj-$(CONFIG_FB_KYRO)             += kyro/ cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_SAVAGE)		  += savage/ cfbfillrect.o cfbcopyarea.o \
				     cfbimgblt.o
obj-$(CONFIG_FB_I810)             += cfbcopyarea.o cfbfillrect.o cfbimgblt.o \
				     vgastate.o
obj-$(CONFIG_FB_INTEL)            += cfbfillrect.o cfbcopyarea.o \
	                             cfbimgblt.o

obj-$(CONFIG_FB_RADEON_OLD)	  += radeonfb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_NEOMAGIC)         += neofb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o vgastate.o
obj-$(CONFIG_FB_VIRGE)            += virgefb.o
obj-$(CONFIG_FB_3DFX)             += tdfxfb.o cfbimgblt.o
ifneq ($(CONFIG_FB_3DFX_ACCEL),y)
obj-$(CONFIG_FB_3DFX)             += cfbfillrect.o cfbcopyarea.o
endif
obj-$(CONFIG_FB_CONTROL)          += controlfb.o macmodes.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_PLATINUM)         += platinumfb.o macmodes.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_VALKYRIE)         += valkyriefb.o macmodes.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_CT65550)          += chipsfb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_IMSTT)            += imsttfb.o cfbimgblt.o
obj-$(CONFIG_FB_S3TRIO)           += S3triofb.o
obj-$(CONFIG_FB_FM2)              += fm2fb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_TRIDENT)	  += tridentfb.o cfbfillrect.o cfbimgblt.o cfbcopyarea.o
obj-$(CONFIG_FB_STI)              += stifb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_FFB)              += ffb.o sbuslib.o cfbimgblt.o cfbcopyarea.o
obj-$(CONFIG_FB_CG6)              += cg6.o sbuslib.o cfbimgblt.o cfbcopyarea.o
obj-$(CONFIG_FB_CG3)              += cg3.o sbuslib.o cfbimgblt.o cfbcopyarea.o \
				     cfbfillrect.o
obj-$(CONFIG_FB_BW2)              += bw2.o sbuslib.o cfbimgblt.o cfbcopyarea.o \
				     cfbfillrect.o
obj-$(CONFIG_FB_CG14)             += cg14.o sbuslib.o cfbimgblt.o cfbcopyarea.o \
				     cfbfillrect.o
obj-$(CONFIG_FB_P9100)            += p9100.o sbuslib.o cfbimgblt.o cfbcopyarea.o \
				     cfbfillrect.o
obj-$(CONFIG_FB_TCX)              += tcx.o sbuslib.o cfbimgblt.o cfbcopyarea.o \
				     cfbfillrect.o
obj-$(CONFIG_FB_LEO)              += leo.o sbuslib.o cfbimgblt.o cfbcopyarea.o \
				     cfbfillrect.o
obj-$(CONFIG_FB_SGIVW)            += sgivwfb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_ACORN)            += acornfb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_ATARI)            += atafb.o
obj-$(CONFIG_FB_MAC)              += macfb.o macmodes.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_HGA)              += hgafb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o 
obj-$(CONFIG_FB_IGA)              += igafb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_APOLLO)           += dnfb.o cfbfillrect.o cfbimgblt.o
obj-$(CONFIG_FB_Q40)              += q40fb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_TGA)              += tgafb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_HP300)            += hpfb.o cfbfillrect.o cfbimgblt.o
obj-$(CONFIG_FB_G364)             += g364fb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_SA1100)           += sa1100fb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_SUN3)             += sun3fb.o
obj-$(CONFIG_FB_HIT)              += hitfb.o cfbfillrect.o cfbimgblt.o
obj-$(CONFIG_FB_EPSON1355)	  += epson1355fb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_PVR2)             += pvr2fb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_VOODOO1)          += sstfb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_ARMCLCD)	  += amba-clcd.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_68328)            += 68328fb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_GBE)              += gbefb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_CIRRUS)		  += cirrusfb.o cfbfillrect.o cfbimgblt.o cfbcopyarea.o
obj-$(CONFIG_FB_ASILIANT)	  += asiliantfb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_PXA)		  += pxafb.o cfbimgblt.o cfbcopyarea.o cfbfillrect.o
obj-$(CONFIG_FB_W100)		   += w100fb.o cfbimgblt.o cfbcopyarea.o cfbfillrect.o
obj-$(CONFIG_FB_AU1100)		  += au1100fb.o fbgen.o
obj-$(CONFIG_FB_PMAG_AA)	  += pmag-aa-fb.o  cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_PMAG_BA)	  += pmag-ba-fb.o  cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_PMAGB_B)	  += pmagb-b-fb.o  cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_MAXINE)		  += maxinefb.o  cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_TX3912)		  += tx3912fb.o  cfbfillrect.o cfbcopyarea.o cfbimgblt.o

# Platform or fallback drivers go here
obj-$(CONFIG_FB_VESA)             += vesafb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
obj-$(CONFIG_FB_VGA16)            += vga16fb.o cfbfillrect.o cfbcopyarea.o \
	                             cfbimgblt.o vgastate.o
obj-$(CONFIG_FB_OF)               += offb.o cfbfillrect.o cfbimgblt.o cfbcopyarea.o

# the test framebuffer is last
obj-$(CONFIG_FB_VIRTUAL)          += vfb.o cfbfillrect.o cfbcopyarea.o cfbimgblt.o
