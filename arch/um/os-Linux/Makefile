# 
# Copyright (C) 2000 Jeff Dike (jdike@karaya.com)
# Licensed under the GPL
#

obj-y = elf_aux.o file.o process.o signal.o time.o tty.o user_syms.o drivers/ \
	sys-$(SUBARCH)/

USER_OBJS := elf_aux.o file.o process.o signal.o time.o tty.o
USER_OBJS := $(foreach file,$(USER_OBJS),$(obj)/$(file))

$(USER_OBJS) : %.o: %.c
	$(CC) $(CFLAGS_$(notdir $@)) $(USER_CFLAGS) -c -o $@ $<

CFLAGS_user_syms.o += -DSUBARCH_$(SUBARCH)
