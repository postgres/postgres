#! /bin/sh

# $1 = path to tkConfig.sh ; $2 = output file

if test x"$1" = x; then
    echo "$0: No tkConfig.sh file specified. Did you use \`configure --with-tcl --with-x'?" 1>&2
    exit 1
fi

# Source the file to obtain the correctly expanded variable definitions
. "$1"

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
