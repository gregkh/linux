#
# Makefile for arch/m68knommu/platform/68VZ328/ucdimm.
#

obj-y := config.o

EXTRA_TARGETS := bootlogo.rh crt0_$(MODEL).o

$(obj)/bootlogo.rh: $(src)/../../68EZ328/bootlogo.h
	perl $(src)/../../68328/bootlogo.pl < $(src)/../../68EZ328/bootlogo.h \
		> $(obj)/bootlogo.rh

