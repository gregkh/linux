# 
# Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
# Licensed under the GPL
#

obj-y = proxy.o ptrace.o sysdep.o wait.o

USER_OBJS := $(foreach file,$(obj-y),$(src)/$(file))

$(USER_OBJS) : %.o: %.c
	$(CC) $(CFLAGS_$(notdir $@)) $(USER_CFLAGS) -c -o $@ $<
