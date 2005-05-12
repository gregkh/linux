#
# Makefile for arch/m68knommu/platform/68328.
#

obj-$(CONFIG_M68328)	+= entry.o ints.o config.o
obj-$(CONFIG_M68EZ328)	+= entry.o ints.o
obj-$(CONFIG_M68VZ328)	+= entry.o ints.o

ifeq ($(CONFIG_M68328),y)
extra-y := $(BOARD)/bootlogo.rh $(BOARD)/crt0_$(MODEL).o
endif

$(obj)/$(BOARD)/bootlogo.rh: $(src)/bootlogo.h
	perl $(src)/bootlogo.pl < $(src)/bootlogo.h > $(obj)/$(BOARD)/bootlogo.rh

