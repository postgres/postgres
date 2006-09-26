#! /bin/sh
# $PostgreSQL: pgsql/src/interfaces/ecpg/test/pg_regress.sh,v 1.15 2006/09/26 07:56:56 meskes Exp $

me=`basename $0`

message(){
    _dashes='==============' # 14
    _spaces='                                      ' # 38
    _msg=`echo "$1$_spaces" | cut -c 1-38`
    echo "$_dashes $_msg $_dashes"
}

build_help(){
help="\
PostgreSQL regression test driver

Usage: $me [options...] [extra tests...]

Options:
  --dbname=DB               use database DB (default \`regression')
  --debug                   turn on debug mode in programs that are run
  --inputdir=DIR            take input files from DIR (default \`.')
  --load-language=lang      load the named language before running the
                            tests; can appear multiple times
  --max-connections=N       maximum number of concurrent connections
                            (default is 0 meaning unlimited)
  --multibyte=ENCODING      use ENCODING as the multibyte encoding, and
                            also run a test by the same name
  --outputdir=DIR           place output files in DIR (default \`.')
  --temp-install[=DIR]      create a temporary installation (in DIR)
  --no-locale               use C locale
$1
Options for \`temp-install' mode:
  --top-builddir=DIR        (relative) path to top level build directory
  --temp-port=PORT          port number to start temp postmaster on
  --listen-on-tcp           listen on the tcp port as well

Options for using an existing installation:
  --host=HOST               use postmaster running on HOST
  --port=PORT               use postmaster running at PORT
  --user=USER               connect as USER

The exit status is 0 if all tests passed, 1 if some tests failed, and 2
if the tests could not be run for some reason.

Report bugs to <pgsql-bugs@postgresql.org>."
}

init_vars(){
	: ${TMPDIR=/tmp}
	TMPFILE=$TMPDIR/pg_regress.$$

	# ----------
	# Initialize default settings
	# ----------

	: ${inputdir=.}
	: ${outputdir=.}

	libdir='@libdir@'
	bindir='@bindir@'
	datadir='@datadir@'
	host_platform='@host_tuple@'
	enable_shared='@enable_shared@'
	GCC=@GCC@
	VERSION=@VERSION@

	if [ "$GCC" = yes ]; then
	    compiler=gcc
	else
	    compiler=cc
	fi

	unset mode
	unset schedule
	unset debug
	unset nolocale
	unset top_builddir
	unset temp_install
	unset multibyte

	dbname=regression
	hostname=localhost
	maxconnections=0
	temp_port=65432
	load_langs=""
	listen_on_tcp=no

	: ${GMAKE='@GMAKE@'}
}

parse_general_options(){
# ----------
# Parse command line options
# ----------

while [ "$#" -gt 0 ]
do
    case $1 in
        --help|-\?)
                echo "$help"
                exit 0;;
        --version)
                echo "pg_regress (PostgreSQL $VERSION)"
                exit 0;;
        --dbname=*)
                dbname=`expr "x$1" : "x--dbname=\(.*\)"`
                shift;;
        --debug)
                debug=yes
                shift;;
        --inputdir=*)
                inputdir=`expr "x$1" : "x--inputdir=\(.*\)"`
                shift;;
        --listen-on-tcp)
                listen_on_tcp=yes
                shift;;
        --load-language=*)
                lang=`expr "x$1" : "x--load-language=\(.*\)"`
                load_langs="$load_langs $lang"
                unset lang
                shift;;
        --multibyte=*)
                multibyte=`expr "x$1" : "x--multibyte=\(.*\)"`
                shift;;
        --no-locale)
                nolocale=yes
                shift;;
        --temp-install)
                temp_install=./tmp_check
                shift;;
        --temp-install=*)
                temp_install=`expr "x$1" : "x--temp-install=\(.*\)"`
                shift;;
        --max-connections=*)
                maxconnections=`expr "x$1" : "x--max-connections=\(.*\)"`
                shift;;
        --outputdir=*)
                outputdir=`expr "x$1" : "x--outputdir=\(.*\)"`
                shift;;
        --top-builddir=*)
                top_builddir=`expr "x$1" : "x--top-builddir=\(.*\)"`
                shift;;
        --temp-port=*)
                temp_port=`expr "x$1" : "x--temp-port=\(.*\)"`
                shift;;
        --host=*)
                PGHOST=`expr "x$1" : "x--host=\(.*\)"`
                export PGHOST
                unset PGHOSTADDR
                shift;;
        --port=*)
                PGPORT=`expr "x$1" : "x--port=\(.*\)"`
                export PGPORT
                shift;;
        --user=*)
                PGUSER=`expr "x$1" : "x--user=\(.*\)"`
                export PGUSER
                shift;;
        -*)
                # on error, this will not return but exit
                parse_special_options "$1"
                shift;;
        *)
                extra_tests="$extra_tests $1"
                shift;;
    esac
done
}



setup_environment_variables(){

	# This function has two parts. Part 1 sets/unsets environment variables
	# independently of what options the script receives.
	# Part 2 later sets environment variables with respect to the
	# options given.

	# =======
	# PART 1: Options independent stuff goes here
	# =======


	# ----------
	# Unset locale settings
	# ----------

	unset LC_COLLATE LC_CTYPE LC_MONETARY LC_MESSAGES LC_NUMERIC LC_TIME LC_ALL LANG LANGUAGE

	# On Windows the default locale may not be English, so force it
	case $host_platform in
	    *-*-cygwin*|*-*-mingw32*)
		LANG=en
		export LANG
		;;
	esac

	# ----------
	# On some platforms we can't use Unix sockets.
	# ----------

	case $host_platform in
	    *-*-cygwin* | *-*-mingw32*)
		listen_on_tcp=yes
	esac

	# ----------
	# Set up diff to ignore horizontal white space differences.
	# ----------

	case $host_platform in
	    *-*-sco3.2v5*)
		DIFFFLAGS=-b;;
	    *)
		DIFFFLAGS=-w;;
	esac

	# ----------
	# Check for echo -n vs echo \c
	# ----------

	if echo '\c' | grep c >/dev/null 2>&1; then
	    ECHO_N='echo -n'
	    ECHO_C=''
	else
	    ECHO_N='echo'
	    ECHO_C='\c'
	fi

	# ----------
	# Set backend timezone and datestyle explicitly
	#
	# To pass the horology test in its current form, the postmaster must be
	# started with PGDATESTYLE=ISO, while the frontend must be started with
	# PGDATESTYLE=Postgres.  We set the postmaster values here and change
	# to the frontend settings after the postmaster has been started.
	# ----------

	PGTZ='PST8PDT'; export PGTZ
	PGDATESTYLE='ISO, MDY'; export PGDATESTYLE

	# ----------
	# Set up SQL shell for the test.
	# ----------

	psql_test_options="-a -q -X $psql_options"



	# =======
	# PART 2: Options dependent stuff goes here
	# =======

	LOGDIR=$outputdir/log

	# ----------
	# warn of Cygwin likely failure if maxconnections = 0
	# and we are running parallel tests
	# ----------

	case $host_platform in
	    *-*-cygwin*)
		case "$schedule" in
		    *parallel_schedule*)
			if [ $maxconnections -eq 0 ] ; then
			    echo Using unlimited parallel connections is likely to fail or hang on Cygwin.
			    echo Try \"$me --max-connections=n\" or \"gmake MAX_CONNECTIONS=n check\"
			    echo with n = 5 or 10 if this happens.
			    echo
			fi
			;;
		esac
		;;
	esac

	# ----------
	# Set up multibyte environment
	# ----------

	if [ -n "$multibyte" ]; then
	    PGCLIENTENCODING=$multibyte
	    export PGCLIENTENCODING
	    encoding_opt="-E $multibyte"
	else
	    unset PGCLIENTENCODING
	fi
}

do_temp_install(){
    if echo x"$temp_install" | grep -v '^x/' >/dev/null 2>&1; then
        temp_install="`pwd`/$temp_install"
    fi

    bindir=$temp_install/install/$bindir
    libdir=$temp_install/install/$libdir
    datadir=$temp_install/install/$datadir
    PGDATA=$temp_install/data

    if [ "$unix_sockets" = no ]; then
        PGHOST=$hostname
        export PGHOST
        unset PGHOSTADDR
    else
        unset PGHOST
        unset PGHOSTADDR
    fi

    # since Makefile isn't very bright, check for out-of-range temp_port
    if [ "$temp_port" -ge 1024 -a "$temp_port" -le 65535 ] ; then
	PGPORT=$temp_port
    else
	PGPORT=65432
    fi
    export PGPORT

    # Get rid of environment stuff that might cause psql to misbehave
    # while contacting our temp installation
    unset PGDATABASE PGUSER PGSERVICE PGSSLMODE PGREQUIRESSL PGCONNECT_TIMEOUT

    # ----------
    # Set up shared library paths, needed by psql and pg_encoding
    # (if you run multibyte).  LD_LIBRARY_PATH covers many platforms.
    # DYLD_LIBRARY_PATH works on Darwin, and maybe other Mach-based systems.
    # LIBPATH is for AIX.
    # Feel free to account for others as well.
    # ----------

    if [ -n "$LD_LIBRARY_PATH" ]; then
        LD_LIBRARY_PATH="$libdir:$LD_LIBRARY_PATH"
    else
        LD_LIBRARY_PATH=$libdir
    fi
    export LD_LIBRARY_PATH

    if [ -n "$DYLD_LIBRARY_PATH" ]; then
        DYLD_LIBRARY_PATH="$libdir:$DYLD_LIBRARY_PATH"
    else
        DYLD_LIBRARY_PATH=$libdir
    fi
    export DYLD_LIBRARY_PATH

    if [ -n "$LIBPATH" ]; then
        LIBPATH="$libdir:$LIBPATH"
    else
        LIBPATH=$libdir
    fi
    export LIBPATH

    # ----------
    # Windows needs shared libraries in PATH. (Only those linked into
    # executables, not dlopen'ed ones)
    # ----------
    case $host_platform in
        *-*-cygwin*|*-*-mingw32*)
            PATH=$libdir:$PATH
            export PATH
            ;;
    esac

    if [ -d "$temp_install" ]; then
        message "removing existing temp installation"
        rm -rf "$temp_install"
    fi

    message "creating temporary installation"
    if [ ! -d "$LOGDIR" ]; then
        mkdir -p "$LOGDIR" || { (exit 2); exit; }
    fi
    $GMAKE -C "$top_builddir" DESTDIR="$temp_install/install" install with_perl=no with_python=no >"$LOGDIR/install.log" 2>&1

    if [ $? -ne 0 ]
    then
        echo
        echo "$me: installation failed"
        echo "Examine $LOGDIR/install.log for the reason."
        echo
        (exit 2); exit
    fi

    message "initializing database system"
    [ "$debug" = yes ] && initdb_options="--debug"
    [ "$nolocale" = yes ] && initdb_options="$initdb_options --no-locale"
    "$bindir/initdb" -D "$PGDATA" -L "$datadir" --noclean $initdb_options >"$LOGDIR/initdb.log" 2>&1

    if [ $? -ne 0 ]
    then
        echo
        echo "$me: initdb failed"
        echo "Examine $LOGDIR/initdb.log for the reason."
        echo
        (exit 2); exit
    fi


    # ----------
    # Start postmaster
    # ----------

    message "starting postmaster"
    [ "$debug" = yes ] && postmaster_options="$postmaster_options -d 5"
    if [ "$listen_on_tcp" = yes ]; then
        postmaster_options="$postmaster_options -c listen_addresses=$hostname"
    else
        postmaster_options="$postmaster_options -c listen_addresses="
    fi
    "$bindir/postmaster" -D "$PGDATA" -F $postmaster_options >"$LOGDIR/postmaster.log" 2>&1 &
    postmaster_pid=$!

    # Wait till postmaster is able to accept connections (normally only
    # a second or so, but Cygwin is reportedly *much* slower).  Don't
    # wait forever, however.
    i=0
    max=60
    until "$bindir/psql" -X $psql_options postgres </dev/null 2>/dev/null
    do
        i=`expr $i + 1`
        if [ $i -ge $max ]
        then
            break
        fi
        if kill -0 $postmaster_pid >/dev/null 2>&1
        then
            : still starting up
        else
            break
        fi
        sleep 1
    done

    if kill -0 $postmaster_pid >/dev/null 2>&1
    then
        echo "running on port $PGPORT with pid $postmaster_pid"
    else
        echo
        echo "$me: postmaster did not start"
        echo "Examine $LOGDIR/postmaster.log for the reason."
        echo
        (exit 2); exit
    fi
}

dont_temp_install(){
    # ----------
    # Windows needs shared libraries in PATH. (Only those linked into
    # executables, not dlopen'ed ones)
    # ----------
    case $host_platform in
        *-*-cygwin*|*-*-mingw32*)
            PATH=$libdir:$PATH
            export PATH
            ;;
    esac

    if [ -n "$PGPORT" ]; then
        port_info="port $PGPORT"
    else
        port_info="default port"
    fi

    if [ -n "$PGHOST" ]; then
        echo "(using postmaster on $PGHOST, $port_info)"
    else
        if [ "$unix_sockets" = no ]; then
            echo "(using postmaster on localhost, $port_info)"
        else
            echo "(using postmaster on Unix socket, $port_info)"
        fi
    fi
}

setup_client_environment_variables(){
	PGDATESTYLE='Postgres'
	export PGDATESTYLE
}

# ----------
# Exit trap to remove temp file and shut down postmaster
# ----------

# Note:  There are some stupid shells (even among recent ones) that
# ignore the argument to exit (as in `exit 1') if there is an exit
# trap.  The trap (and thus the shell script) will then always exit
# with the result of the last shell command before the `exit'.  Hence
# we have to write `(exit x); exit' below this point.

exit_trap(){ 
    savestatus=$1
    if [ -n "$postmaster_pid" ]; then
        kill -2 "$postmaster_pid"
        wait "$postmaster_pid"
        unset postmaster_pid
    fi
    rm -f "$TMPFILE" && exit $savestatus
}

sig_trap() {
    savestatus=$1
    echo; echo "caught signal"
    if [ -n "$postmaster_pid" ]; then
        echo "signalling fast shutdown to postmaster with pid $postmaster_pid"
        kill -2 "$postmaster_pid"
        wait "$postmaster_pid"
        unset postmaster_pid
    fi
    (exit $savestatus); exit
}

setup_database(){
	# this receives the name of the database to set up as its argument
	"$bindir/psql" -q -X $psql_options -c "\
	alter database \"$1\" set lc_messages to 'C';
	alter database \"$1\" set lc_monetary to 'C';
	alter database \"$1\" set lc_numeric to 'C';
	alter database \"$1\" set lc_time to 'C';" "$1"
	if [ $? -ne 0 ]; then
	    echo "$me: could not set database default locales"
	    (exit 2); exit
	fi

	# ----------
	# Install any requested PL languages
	# ----------

	if [ "$enable_shared" = yes ]; then
	    for lang in xyzzy $load_langs ; do    
		if [ "$lang" != "xyzzy" ]; then
		    message "installing $lang"
		    "$bindir/createlang" $psql_options $lang "$1"
		    if [ $? -ne 0 ] && [ $? -ne 2 ]; then
			echo "$me: createlang $lang failed"
			(exit 2); exit
		    fi
		fi
	    done
	fi
}

drop_database(){
	message "dropping database \"$1\""
	"$bindir/dropdb" $psql_options "$1"
}

create_database(){
	# ----------
	# We use template0 so that any installation-local cruft in template1
	# will not mess up the tests.
	# ----------

	message "creating database \"$1\""
	"$bindir/createdb" $encoding_opt $psql_options --template template0 "$1"
	if [ $? -ne 0 ]; then
	    echo "$me: createdb failed"
	    (exit 2); exit
	fi

	setup_database "$1"
}

database_cleanup(){
	# ----------
	# Remove regressuser* and regressgroup* user accounts.
	# ----------

	message "dropping regression test user accounts"
	"$bindir/psql" -q -X $psql_options -c 'DROP GROUP regressgroup1; DROP GROUP regressgroup2; DROP USER regressuser1, regressuser2, regressuser3, regressuser4;' $dbname 2>/dev/null
	if [ $? -eq 2 ]; then
	    echo "$me: could not drop user accounts"
	    (exit 2); exit
	fi
}

postmaster_shutdown(){
	# ----------
	# Server shutdown
	# ----------

	if [ -n "$postmaster_pid" ]; then
	    message "shutting down postmaster"
	    "$bindir/pg_ctl" -s -D "$PGDATA" stop
	    wait "$postmaster_pid"
	    unset postmaster_pid
	fi
}

evaluate(){
	# ----------
	# Evaluation
	# ----------

	count_total=`cat "$result_summary_file" | grep '\.\.\.' | wc -l | sed 's/ //g'`
	count_ok=`cat "$result_summary_file" | grep '\.\.\. ok' | wc -l | sed 's/ //g'`
	count_failed=`cat "$result_summary_file" | grep '\.\.\. FAILED' | wc -l | sed 's/ //g'`
	count_ignored=`cat "$result_summary_file" | grep '\.\.\. failed (ignored)' | wc -l | sed 's/ //g'`

	echo
	if [ $count_total -eq $count_ok ]; then
	    msg="All $count_total tests passed."
	    result=0
	elif [ $count_failed -eq 0 ]; then
	    msg="$count_ok of $count_total tests passed, $count_ignored failed test(s) ignored."
	    result=0
	elif [ $count_ignored -eq 0 ]; then
	    msg="$count_failed of $count_total tests failed."
	    result=1
	else
	    msg="`expr $count_failed + $count_ignored` of $count_total tests failed, $count_ignored of these failures ignored."
	    result=1
	fi

	dashes=`echo " $msg " | sed 's/./=/g'`
	echo "$dashes"
	echo " $msg "
	echo "$dashes"
	echo

	if [ -s "$diff_file" ]; then
	    echo "The differences that caused some tests to fail can be viewed in the"
	    echo "file \`$diff_file'.  A copy of the test summary that you see"
	    echo "above is saved in the file \`$result_summary_file'."
	    echo
	else
	    rm -f "$diff_file" "$result_summary_file"
	fi
}

additional_regress_options=""

build_help "$additional_regress_options"
init_vars

parse_special_options(){
# no special options so far
    case $1 in
        -*)
                echo "$me: invalid argument $1" 1>&2
                exit 2;;
    esac
}

# this will call parse_special_options from above
parse_general_options $*

# ----------
# Set up the environment variables (some of them depend on the parameters)
# ----------
setup_environment_variables

trap 'exit_trap $?' 0
trap 'sig_trap $?' 1 2 13 15

if [ x"$temp_install" != x"" ]
then
	do_temp_install
	#PGPORT=$temp_port; export PGPORT
else # not temp-install
	dont_temp_install
fi

# ----------
# Postmaster is started, now we can change some environment variables for the
# client
# ----------

setup_client_environment_variables

# set up the dbs we use for ecpg regression tests
drop_database "$dbname"
create_database "$dbname"
drop_database connectdb
create_database connectdb

# ----------
# Let's go
# ----------

message "running regression test queries"

outputdir="results"

if [ ! -d "$outputdir" ]; then
    mkdir -p "$outputdir" || { (exit 2); exit; }
fi
#result_summary_file=$outputdir/regression.out
#diff_file=$outputdir/regression.diffs

#cat /dev/null >"$result_summary_file"
#cat /dev/null >"$diff_file"

# we also have different users for ecpg regression diffs (need them for testing
# connects)
echo "$bindir/createuser" $psql_options -R -S -D -q regressuser1
"$bindir/createuser" $psql_options -R -S -D -q regressuser1
if [ $? -ne 0 ]; then
	echo Could not create user regressuser1
fi
echo "$bindir/createuser" $psql_options -R -S -D -q connectuser
"$bindir/createuser" $psql_options -R -S -D -q connectuser
if [ $? -ne 0 ]; then
	echo Could not create user connectuser
fi
# to test username = dbname 
echo "$bindir/createuser" $psql_options -R -S -D -q connectdb
"$bindir/createuser" $psql_options -R -S -D -q connectdb
if [ $? -ne 0 ]; then
	echo Could not create user connectdb
fi

# this variable prevents that the PID gets included in the logfiles
ECPG_REGRESSION=1; export ECPG_REGRESSION
LD_LIBRARY_PATH=$libdir:$LD_LIBRARY_PATH; export LD_LIBRARY_PATH

DIFFPRETTYFLAGS="$DIFFFLAGS -C3"
FAILNUM=""

rm -f regression.diffs

for i in \
         connect/*.pgc \
         compat_informix/*.pgc \
         preproc/*.pgc \
         pgtypeslib/*.pgc \
         sql/*.pgc \
         thread/*.pgc; do

	formatted=`echo $i | awk '{printf "%-38.38s", $1;}'`
	$ECHO_N "testing $formatted ... $ECHO_C"

	# connect/test1.pgc uses tcp to connect to the server. We run this test
	# only if called with --listen-on-tcp
	if [ $listen_on_tcp = no ] && [ "$i" = "connect/test1.pgc" ]; then
		echo skipped
		continue;
	fi

	runprg=`echo $i | sed -e 's,\.pgc$,,'`
	outprg=`echo $runprg | sed -e's/\//-/'`

	case $host_platform in
	    *-*-mingw32*)
		PLATFORM_TAG="-MinGW32"
		;;
	esac

	outfile_stderr="$outputdir/$outprg.stderr"
	outfile_stdout="$outputdir/$outprg.stdout"
	outfile_source="$outputdir/$outprg.c"
	cp $runprg.c "$outfile_source"
	# echo "$runprg > $outfile_stdout 2> $outfile_stderr"
	$runprg > "$outfile_stdout" 2> "$outfile_stderr"

	# If we don't run on the default port we'll get different output
	# so tweak output files and replace the port number (we put a warning
	# but the price to pay is that we have to tweak the files every time
	# now not only if the port differs from the standard port).
	if [ "$i" = "connect/test1.pgc" ]; then
		# can we use sed -i on all platforms?
		for f in "$outfile_stderr" "$outfile_stdout" "$outfile_source"; do
			mv $f $f.tmp
			echo >> $f
			echo "THE PORT NUMBER MIGHT HAVE BEEN CHANGED BY THE REGRESSION SCRIPT" >> $f
			echo >> $f
			# MinGW could return such a line:
			# "could not connect to server: Connection refused (0x0000274D/10061)"
			cat $f.tmp | sed -e s,$PGPORT,55432,g | sed -e "s,could not connect to server: Connection refused (0x.*/.*),could not connect to server: Connection refused,g" >> $f
			rm $f.tmp
		done
	fi

	mv "$outfile_source" "$outfile_source.tmp"
	cat "$outfile_source.tmp" | sed -e 's,^\(#line [0-9]*\) ".*/\([^/]*\)",\1 "\2",' > "$outfile_source"
	rm "$outfile_source.tmp"

	expected_stderr="expected/$outprg$PLATFORM_TAG.stderr"
	if [ ! -f "$expected_stderr" ]; then
		expected_stderr="expected/$outprg.stderr"
	fi
	expected_stdout="expected/$outprg$PLATFORM_TAG.stdout"
	if [ ! -f "$expected_stdout" ]; then
		expected_stdout="expected/$outprg.stdout"
	fi
	# the source should be identical on all platforms
	expected_source="expected/$outprg.c"

	DIFFER=""
	diff $DIFFFLAGS "$expected_stderr" "$outfile_stderr" > /dev/null 2>&1
	if [ $? != 0 ]; then
		DIFFER="$DIFFER, log"
		diff $DIFFPRETTYFLAGS "$expected_stderr" "$outfile_stderr" >> regression.diffs 2>&1
	fi

	diff $DIFFFLAGS "$expected_stdout" "$outfile_stdout" > /dev/null 2>&1
	if [ $? != 0 ]; then
		DIFFER="$DIFFER, output"
		diff $DIFFPRETTYFLAGS "$expected_stdout" "$outfile_stdout" >> regression.diffs 2>&1
	fi

	diff $DIFFFLAGS "$expected_source" "$outputdir"/$outprg.c > /dev/null 2>&1
	if [ $? != 0 ]; then
		DIFFER="$DIFFER, source"
		diff $DIFFPRETTYFLAGS "$expected_source" "$outputdir"/$outprg.c >> regression.diffs 2>&1
	fi

	DIFFER=`echo $DIFFER | sed -e 's/^, //'`
	if [ "x$DIFFER" = "x" ]; then
		echo ok
	else
		echo "FAILED ($DIFFER)"
		# some sh's don't know about $((x+1))
		FAILNUM=x$FAILNUM
	fi
done

postmaster_shutdown

# FAILNUM is empty if no test has failed
[ x"$FAILNUM" = x"" ] && exit 0
(exit 1); exit

