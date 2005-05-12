#
# Copyright (C) 2000 Jeff Dike (jdike@karaya.com)
# Licensed under the GPL
#

obj-$(CONFIG_MODE_SKAS) = registers.o

USER_OBJS := $(foreach file,$(obj-y),$(obj)/$(file))

$(USER_OBJS) : %.o: %.c
	$(CC) $(CFLAGS_$(notdir $@)) $(USER_CFLAGS) -c -o $@ $<
