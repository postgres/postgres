#!/bin/sh

set -e
set -x

# The default filesystem on freebsd gcp images is very slow to run tests on,
# due to its 32KB block size
#
# XXX: It'd probably better to fix this in the image, using something like
# https://people.freebsd.org/~lidl/blog/re-root.html

# fix backup partition table after resize
gpart recover da0
gpart show da0
# kill swap, so we can delete a partition
swapoff -a || true
# (apparently we can only have 4!?)
gpart delete -i 3 da0
gpart add -t freebsd-ufs -l data8k -a 4096 da0
gpart show da0
newfs -U -b 8192 /dev/da0p3

# Migrate working directory
du -hs $CIRRUS_WORKING_DIR
mv $CIRRUS_WORKING_DIR $CIRRUS_WORKING_DIR.orig
mkdir $CIRRUS_WORKING_DIR
mount -o noatime /dev/da0p3 $CIRRUS_WORKING_DIR
cp -r $CIRRUS_WORKING_DIR.orig/* $CIRRUS_WORKING_DIR/
