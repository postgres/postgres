#!/bin/sh
#
# mkldexport
#	create an AIX exports file from an object file
#
# Usage:
#	mkldexport objectfile [location]
# where
#	objectfile is the current location of the object file.
#	location is the eventual (installed) location of the 
#		object file (if different from the current
#		working directory).
#
# /usr/local/devel/postgres-v4r2/src/tools/mkldexport/RCS/mkldexport.sh,v 1.2 1994/03/13 04:59:12 aoki Exp
#
CMDNAME=`basename $0`
if [ -z "$1" ]; then
	echo "Usage: $CMDNAME object [location]"
	exit 1
fi
OBJNAME=`basename $1`
if [ "`basename $OBJNAME`" != "`basename $OBJNAME .o`" ]; then
	OBJNAME=`basename $OBJNAME .o`.so
fi
if [ -z "$2" ]; then
	echo '#!'
else
	echo '#!' $2/$OBJNAME
fi
/usr/ucb/nm -g $1 | \
	egrep ' [TD] ' | \
	sed -e 's/.* //' | \
	egrep -v '\$' | \
	sed -e 's/^[.]//' | \
	sort | \
	uniq
