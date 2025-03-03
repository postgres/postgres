#!/bin/sh
# Move working directory into a RAM disk for better performance.

set -e
set -x

mv $CIRRUS_WORKING_DIR $CIRRUS_WORKING_DIR.orig
mkdir $CIRRUS_WORKING_DIR

case "`uname`" in
  FreeBSD|NetBSD)
    mount -t tmpfs tmpfs $CIRRUS_WORKING_DIR
    ;;
  OpenBSD)
    umount /dev/sd0j # unused /usr/obj partition
    printf "m j\n\n\nswap\nw\nq\n" | disklabel -E sd0
    swapon /dev/sd0j
    mount -t mfs -o rw,noatime,nodev,-s=8000000 swap $CIRRUS_WORKING_DIR
    ;;
esac

cp -a $CIRRUS_WORKING_DIR.orig/. $CIRRUS_WORKING_DIR/
