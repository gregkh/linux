# Makefile for the TurboPAM ISDN device driver

# Each configuration option enables a list of files.

obj-$(CONFIG_ISDN_DRV_TPAM)	+= tpam.o

# Multipart objects.

tpam-y				:= tpam_main.o tpam_nco.o tpam_memory.o \
				   tpam_commands.o tpam_queues.o tpam_hdlc.o \
				   tpam_crcpc.o
