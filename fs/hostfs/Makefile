#
# Copyright (C) 2000 Jeff Dike (jdike@karaya.com)
# Licensed under the GPL
#

# struct stat64 changed the inode field name between 2.2 and 2.4 from st_ino
# to __st_ino.  It stayed in the same place, so as long as the correct name
# is used, hostfs compiled on 2.2 should work on 2.4 and vice versa.

STAT64_INO_FIELD := $(shell grep -q __st_ino /usr/include/bits/stat.h && \
				echo __)st_ino

hostfs-objs := hostfs_kern.o hostfs_user.o

obj-y =
obj-$(CONFIG_HOSTFS) += hostfs.o

SINGLE_OBJS = $(foreach f,$(patsubst %.o,%,$(obj-y) $(obj-m)),$($(f)-objs))

USER_OBJS := $(filter %_user.o,$(obj-y) $(obj-m) $(SINGLE_OBJS))
USER_OBJS := $(foreach file,$(USER_OBJS),$(obj)/$(file))

USER_CFLAGS += -DSTAT64_INO_FIELD=$(STAT64_INO_FIELD)

$(USER_OBJS) : %.o: %.c
	$(CC) $(CFLAGS_$(notdir $@)) $(USER_CFLAGS) -c -o $@ $<
