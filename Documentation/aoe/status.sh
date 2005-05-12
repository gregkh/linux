#! /bin/sh
# collate and present sysfs information about AoE storage

set -e
format="%8s\t%8s\t%8s\n"
me=`basename $0`

# printf "$format" device mac netif state

test -z "`mount | grep sysfs`" && {
	echo "$me Error: sysfs is not mounted" 1>&2
	exit 1
}
test -z "`lsmod | grep '^aoe'`" && {
	echo  "$me Error: aoe module is not loaded" 1>&2
	exit 1
}

for d in `ls -d /sys/block/etherd* 2>/dev/null | grep -v p` end; do
	# maybe ls comes up empty, so we use "end"
	test $d = end && continue

	dev=`echo "$d" | sed 's/.*!//'`
	printf "$format" \
		"$dev" \
		"`cat \"$d/netif\"`" \
		"`cat \"$d/state\"`"
done | sort
