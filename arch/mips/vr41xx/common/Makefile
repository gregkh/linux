#
# Makefile for common code of the NEC VR4100 series.
#

obj-y				+= bcu.o cmu.o giu.o icu.o init.o int-handler.o ksyms.o pmu.o rtc.o
obj-$(CONFIG_SERIAL_8250)	+= serial.o
obj-$(CONFIG_VRC4171)		+= vrc4171.o
obj-$(CONFIG_VRC4173)		+= vrc4173.o

EXTRA_AFLAGS := $(CFLAGS)
