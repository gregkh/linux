#
# Makefile for arch/m68knommu/platform/68EZ328.
#

obj-y := config.o

extra-y := $(BOARD)/bootlogo.rh $(BOARD)/crt0_$(MODEL).o

$(obj)/$(BOARD)/bootlogo.rh: $(src)/bootlogo.h
	perl $(src)/../68328/bootlogo.pl < $(src)/bootlogo.h \
		> $(obj)/$(BOARD)/bootlogo.rh

