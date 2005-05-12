# 
# Copyright (C) 2000, 2002 Jeff Dike (jdike@karaya.com)
# Licensed under the GPL
#

ethertap-objs := ethertap_kern.o ethertap_user.o
tuntap-objs := tuntap_kern.o tuntap_user.o

obj-y = 
obj-$(CONFIG_UML_NET_ETHERTAP) += ethertap.o
obj-$(CONFIG_UML_NET_TUNTAP) += tuntap.o

USER_SINGLE_OBJS = $(foreach f,$(patsubst %.o,%,$(obj-y)),$($(f)-objs))

USER_OBJS = $(filter %_user.o,$(obj-y) $(USER_SINGLE_OBJS))
USER_OBJS := $(foreach file,$(USER_OBJS),$(obj)/$(file))

$(USER_OBJS) : %.o: %.c
	$(CC) $(CFLAGS_$(notdir $@)) $(USER_CFLAGS) -c -o $@ $<
