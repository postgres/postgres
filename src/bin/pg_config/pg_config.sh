#! /bin/sh

# This shell script saves various pieces of information about the
# installed version of PostgreSQL.  Packages that interface to
# PostgreSQL can use it to configure their build.
#
# Author:  Peter Eisentraut <peter_e@gmx.net> 
# Public domain

# $Header: /cvsroot/pgsql/src/bin/pg_config/Attic/pg_config.sh,v 1.8 2003/07/23 08:47:25 petere Exp $

me=`basename $0`

# stored configuration values
val_bindir='@bindir@'
val_includedir='@includedir@'
val_includedir_server='@includedir_server@'
val_libdir='@libdir@'
val_pkglibdir='@pkglibdir@'
val_configure="@configure@"
val_version='@version@'

help="\
$me provides information about the installed version of PostgreSQL.

Usage:
  $me OPTION...

Options:
  --bindir              show location of user executables
  --includedir          show location of C header files of the client
                        interfaces
  --includedir-server   show location of C header files for the server
  --libdir              show location of object code libraries
  --pkglibdir           show location of dynamically loadable modules
  --configure           show options given to 'configure' script when
                        PostgreSQL was built
  --version             show the PostgreSQL version, then exit
  --help                show this help, then exit

Report bugs to <pgsql-bugs@postgresql.org>."

advice="\
Try \"$me --help\" for more information."

if test "$#" -eq 0 ; then
    echo "$me: argument required" 1>&2
    echo "$advice" 1>&2
    exit 1
fi

show=

for opt
do
    case "$opt" in
        --bindir)       show="$show \$val_bindir";;
        --includedir)   show="$show \$val_includedir";;
        --includedir-server)
                        show="$show \$val_includedir_server";;
        --libdir)       show="$show \$val_libdir";;
        --pkglibdir)    show="$show \$val_pkglibdir";;
        --configure)    show="$show \$val_configure";;

	--version)      echo "PostgreSQL $val_version"
                        exit 0;;
	--help|-\?)     echo "$help"
                        exit 0;;
        *)              echo "$me: invalid argument: $opt" 1>&2
                        echo "$advice" 1>&2
                        exit 1;;
    esac
done

for thing in $show
do
    eval "echo $thing"
done

# end of pg_config
