#! /bin/sh

# $1 = path to tclConfig.sh ; $2 = output file

# $Header: /cvsroot/pgsql/src/pl/tcl/Attic/mkMakefile.tcldefs.sh,v 1.2 2002/01/23 18:45:41 tgl Exp $

if test x"$1" = x ; then
   echo "$0: No tclConfig.sh file specified. Did you use \`configure --with-tcl'?" 1>&2
   exit 1
fi

# Source the file to obtain the correctly expanded variable definitions
. "$1"

# Suppress bogus soname switch that RedHat RPMs put into tclConfig.sh
TCL_SHLIB_LD=`echo "$TCL_SHLIB_LD" | sed 's/-Wl,-soname[^ ]*//'`

# Read the file a second time as an easy way of getting the list of variable
# definitions to output.
cat "$1" |
    egrep '^TCL_|^TK_' |
    sed 's/^\([^=]*\)=.*$/\1/' |
    while read var
    do
	eval echo "\"$var = \$$var\""
    done > "$2"

exit 0
