#!/bin/sh

set -e
set -x

# fix backup partition table after resize
gpart recover da0
gpart show da0

# delete and re-add swap partition with expanded size
swapoff -a
gpart delete -i 3 da0
gpart add -t freebsd-swap -l swapfs -a 4096 da0
gpart show da0
swapon -a

# create a file system on a memory disk backed by swap, to minimize I/O
mdconfig -a -t swap -s20g -u md1
newfs -b 8192 -U /dev/md1

# migrate working directory
du -hs $CIRRUS_WORKING_DIR
mv $CIRRUS_WORKING_DIR $CIRRUS_WORKING_DIR.orig
mkdir $CIRRUS_WORKING_DIR
mount -o noatime /dev/md1 $CIRRUS_WORKING_DIR
cp -a $CIRRUS_WORKING_DIR.orig/ $CIRRUS_WORKING_DIR/
