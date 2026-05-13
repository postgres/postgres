#!/bin/sh
#
# mkldexport
#	create an AIX exports file from an object file
#
# src/backend/port/aix/mkldexport.sh
#
# Usage:
#	mkldexport objectfile [location]
# where
#	objectfile is the current location of the object file.
#	location is the eventual (installed) location of the
#		object file (if different from the current
#		working directory).
#
# On AIX, executables do not automatically expose their symbols to shared
# modules. Extensions therefore cannot call functions in the main Postgres
# binary unless those symbols are explicitly exported. Unlike other platforms,
# AIX executables are not default symbol providers; each shared module must
# link against an export list that defines which symbols it can use.
#
# The mkldexport.sh script fixes AIX's symbol export issue by generating an
# explicit export list. It uses nm to gather all symbols from the Postgres
# object files, then writes them into the export file. When invoked with ".",
# it outputs #! ., which tells AIX the list applies to the main executable.
# This way, extension modules can link against that list and resolve their
# undefined symbols directly from the Postgres binary.
#

# Search for the nm command binary.
if [ -x /usr/ucb/nm ]
then NM=/usr/ucb/nm
elif [ -x /usr/bin/nm ]
then NM=/usr/bin/nm
elif [ -x /usr/ccs/bin/nm ]
then NM=/usr/ccs/bin/nm
elif [ -x /usr/usg/bin/nm ]
then NM=/usr/usg/bin/nm
else echo "Fatal error: cannot find `nm' ... please check your installation."
     exit 1
fi

# instruct nm to process 64-bit objects
export OBJECT_MODE=64

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
	if [ "$2" = "." ]; then
		# for the base executable (AIX 4.2 and up)
		echo '#! .'
	else
		echo '#!' $2
	fi
fi
$NM -BCg $1 | \
	grep ' [TDB] ' | \
	sed -e 's/.* //' | \
	grep -v '\$' | \
	sed -e 's/^[.]//' | \
	sort | \
	uniq
