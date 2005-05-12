# Makefile for the Linux logos

obj-$(CONFIG_LOGO)			+= logo.o
obj-$(CONFIG_LOGO_LINUX_MONO)		+= logo_linux_mono.o
obj-$(CONFIG_LOGO_LINUX_VGA16)		+= logo_linux_vga16.o
obj-$(CONFIG_LOGO_LINUX_CLUT224)	+= logo_linux_clut224.o
obj-$(CONFIG_LOGO_DEC_CLUT224)		+= logo_dec_clut224.o
obj-$(CONFIG_LOGO_MAC_CLUT224)		+= logo_mac_clut224.o
obj-$(CONFIG_LOGO_PARISC_CLUT224)	+= logo_parisc_clut224.o
obj-$(CONFIG_LOGO_SGI_CLUT224)		+= logo_sgi_clut224.o
obj-$(CONFIG_LOGO_SUN_CLUT224)		+= logo_sun_clut224.o
obj-$(CONFIG_LOGO_SUPERH_MONO)		+= logo_superh_mono.o
obj-$(CONFIG_LOGO_SUPERH_VGA16)		+= logo_superh_vga16.o
obj-$(CONFIG_LOGO_SUPERH_CLUT224)	+= logo_superh_clut224.o

# How to generate logo's

# Use logo-cfiles to retreive list of .c files to be built
logo-cfiles = $(notdir $(patsubst %.$(2), %.c, \
              $(wildcard $(srctree)/$(src)/*$(1).$(2))))


# Mono logos
extra-y += $(call logo-cfiles,_mono,pbm)

quiet_cmd_logo_mono    = MONO    $@
      cmd_logo_mono    = scripts/pnmtologo -t mono -n $*_mono -o $@ $<

$(obj)/%_mono.c: $(src)/%_mono.pbm FORCE
	$(call if_changed,logo_mono)

# VGA16 logos
extra-y += $(call logo-cfiles,_vga16,ppm)

quiet_cmd_logo_vga16   = VGA16   $@
      cmd_logo_vga16   = scripts/pnmtologo -t vga16 -n $*_vga16 -o $@ $<

$(obj)/%_vga16.c: $(src)/%_vga16.ppm FORCE
	$(call if_changed,logo_vga16)

#224 Logos
extra-y += $(call logo-cfiles,_clut224,ppm)

quiet_cmd_logo_clut224 = CLUT224 $@
      cmd_logo_clut224 = scripts/pnmtologo -t clut224 -n $*_clut224 -o $@ $<

$(obj)/%_clut224.c: $(src)/%_clut224.ppm FORCE
	$(call if_changed,logo_clut224)

# Gray 256
extra-y += $(call logo-cfiles,_gray256,pgm)

quiet_cmd_logo_gray256 = GRAY256 $@
      cmd_logo_gray256 = scripts/pnmtologo -t gray256 -n $*_gray256 -o $@ $<

$(obj)/%_gray256.c: $(src)/%_gray256.pgm FORCE
	$(call if_changed,logo_gray256)
