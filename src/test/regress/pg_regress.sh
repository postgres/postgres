#! /bin/sh
# $PostgreSQL: pgsql/src/test/regress/pg_regress.sh,v 1.61 2005/11/01 15:09:11 adunstan Exp $

me=`basename $0`
: ${TMPDIR=/tmp}
TMPFILE=$TMPDIR/pg_regress.$$

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
  --schedule=FILE           use test ordering schedule from FILE
                            (may be used multiple times to concatenate)
  --temp-install[=DIR]      create a temporary installation (in DIR)
  --no-locale               use C locale

Options for \`temp-install' mode:
  --top-builddir=DIR        (relative) path to top level build directory
  --temp-port=PORT          port number to start temp postmaster on

Options for using an existing installation:
  --host=HOST               use postmaster running on HOST
  --port=PORT               use postmaster running at PORT
  --user=USER               connect as USER

The exit status is 0 if all tests passed, 1 if some tests failed, and 2
if the tests could not be run for some reason.

Report bugs to <pgsql-bugs@postgresql.org>."


message(){
    _dashes='==============' # 14
    _spaces='                                      ' # 38
    _msg=`echo "$1$_spaces" | cut -c 1-38`
    echo "$_dashes $_msg $_dashes"
}


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

: ${GMAKE='@GMAKE@'}


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
                echo "pg_regress (PostgreSQL @VERSION@)"
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
        --schedule=*)
                foo=`expr "x$1" : "x--schedule=\(.*\)"`
                schedule="$schedule $foo"
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
                echo "$me: invalid argument $1" 1>&2
                exit 2;;
        *)
                extra_tests="$extra_tests $1"
                shift;;
    esac
done

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
# On some platforms we can't use Unix sockets.
# ----------
case $host_platform in
    *-*-cygwin* | *-*-mingw32* | *-*-qnx* | *beos*)
        unix_sockets=no;;
    *)
        unix_sockets=yes;;
esac


# ----------
# Set up diff to ignore horizontal white space differences.
# ----------

case $host_platform in
    *-*-qnx* | *-*-sco3.2v5*)
        DIFFFLAGS=-b;;
    *)
        DIFFFLAGS=-w;;
esac


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

trap 'exit_trap $?' 0

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

trap 'sig_trap $?' 1 2 13 15



# ----------
# Scan resultmap file to find which platform-specific expected files to use.
# The format of each line of the file is
#         testname/hostplatformpattern=substitutefile
# where the hostplatformpattern is evaluated per the rules of expr(1),
# namely, it is a standard regular expression with an implicit ^ at the start.
# What hostplatformpattern will be matched against is the config.guess output
# followed by either ':gcc' or ':cc' (independent of the actual name of the
# compiler executable).
#
# The tempfile hackery is needed because some shells will run the loop
# inside a subshell, whereupon shell variables set therein aren't seen
# outside the loop :-(
# ----------

cat /dev/null >$TMPFILE
if [ -f "$inputdir/resultmap" ]
then
    while read LINE
    do
        HOSTPAT=`expr "$LINE" : '.*/\(.*\)='`
        if [ `expr "$host_platform:$compiler" : "$HOSTPAT"` -ne 0 ]
        then
            # remove hostnamepattern from line so that there are no shell
            # wildcards in SUBSTLIST; else later 'for' could expand them!
            TESTNAME=`expr "$LINE" : '\(.*\)/'`
            SUBST=`echo "$LINE" | sed 's/^.*=//'`
            echo "$TESTNAME=$SUBST" >> $TMPFILE
        fi
    done <"$inputdir/resultmap"
fi
SUBSTLIST=`cat $TMPFILE`
rm -f $TMPFILE


LOGDIR=$outputdir/log

if [ x"$temp_install" != x"" ]
then
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
    if [ "$unix_sockets" = no ]; then
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

else # not temp-install

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

    message "dropping database \"$dbname\""
    "$bindir/dropdb" $psql_options "$dbname"
    # errors can be ignored
fi


# ----------
# Set up SQL shell for the test.
# ----------

PSQL="$bindir/psql -a -q -X $psql_options"


# ----------
# Set frontend timezone and datestyle explicitly
# ----------

PGTZ='PST8PDT'; export PGTZ
PGDATESTYLE='Postgres, MDY'; export PGDATESTYLE


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


# ----------
# Create the regression database
# We use template0 so that any installation-local cruft in template1
# will not mess up the tests.
# ----------

message "creating database \"$dbname\""
"$bindir/createdb" $encoding_opt $psql_options --template template0 "$dbname"
if [ $? -ne 0 ]; then
    echo "$me: createdb failed"
    (exit 2); exit
fi

"$bindir/psql" -q -X $psql_options -c "\
alter database \"$dbname\" set lc_messages to 'C';
alter database \"$dbname\" set lc_monetary to 'C';
alter database \"$dbname\" set lc_numeric to 'C';
alter database \"$dbname\" set lc_time to 'C';" "$dbname"
if [ $? -ne 0 ]; then
    echo "$me: could not set database default locales"
    (exit 2); exit
fi


# ----------
# Remove regressuser* and regressgroup* user accounts.
# ----------

message "dropping regression test user accounts"
"$bindir/psql" -q -X $psql_options -c 'DROP GROUP regressgroup1; DROP GROUP regressgroup2; DROP USER regressuser1, regressuser2, regressuser3, regressuser4;' $dbname 2>/dev/null
if [ $? -eq 2 ]; then
    echo "$me: could not drop user accounts"
    (exit 2); exit
fi


# ----------
# Install any requested PL languages
# ----------

if [ "$enable_shared" = yes ]; then
    for lang in xyzzy $load_langs ; do    
        if [ "$lang" != "xyzzy" ]; then
            message "installing $lang"
            "$bindir/createlang" $psql_options $lang $dbname
            if [ $? -ne 0 ] && [ $? -ne 2 ]; then
                echo "$me: createlang $lang failed"
                (exit 2); exit
            fi
        fi
    done
fi


# ----------
# Let's go
# ----------

message "running regression test queries"

if [ ! -d "$outputdir/results" ]; then
    mkdir -p "$outputdir/results" || { (exit 2); exit; }
fi
result_summary_file=$outputdir/regression.out
diff_file=$outputdir/regression.diffs

cat /dev/null >"$result_summary_file"
cat /dev/null >"$diff_file"

lno=0
(
    [ "$enable_shared" != yes ] && echo "ignore: plpgsql"
    cat $schedule </dev/null
    for x in $extra_tests; do
        echo "test: $x"
    done
) | sed 's/[ 	]*#.*//g' | \
while read line
do
    # Count line numbers
    lno=`expr $lno + 1`
    [ -z "$line" ] && continue

    set X $line; shift

    if [ x"$1" = x"ignore:" ]; then
        shift
        ignore_list="$ignore_list $@"
        continue
    elif [ x"$1" != x"test:" ]; then
        echo "$me:$schedule:$lno: syntax error"
        (exit 2); exit
    fi

    shift

    # ----------
    # Start tests
    # ----------

    if [ $# -eq 1 ]; then
        # Run a single test
        formatted=`echo $1 | awk '{printf "%-20.20s", $1;}'`
        $ECHO_N "test $formatted ... $ECHO_C"
        ( $PSQL -d "$dbname" <"$inputdir/sql/$1.sql" >"$outputdir/results/$1.out" 2>&1 )&
        wait
    else
        # Start a parallel group
        $ECHO_N "parallel group ($# tests): $ECHO_C"
        if [ $maxconnections -gt 0 ] ; then
            connnum=0
            test $# -gt $maxconnections && $ECHO_N "(in groups of $maxconnections) $ECHO_C"
        fi
        for name do
            ( 
              $PSQL -d "$dbname" <"$inputdir/sql/$name.sql" >"$outputdir/results/$name.out" 2>&1
              $ECHO_N " $name$ECHO_C"
            ) &
            if [ $maxconnections -gt 0 ] ; then
                connnum=`expr \( $connnum + 1 \) % $maxconnections`
                test $connnum -eq 0 && wait
            fi
        done
        wait
        echo
    fi

    # ----------
    # Run diff
    # (We do not want to run the diffs immediately after each test,
    # because they would certainly get corrupted if run in parallel
    # subshells.)
    # ----------

    for name do
        if [ $# -ne 1 ]; then
            formatted=`echo "$name" | awk '{printf "%-20.20s", $1;}'`
            $ECHO_N "     $formatted ... $ECHO_C"
        fi

        # Check list extracted from resultmap to see if we should compare
        # to a system-specific expected file.
        # There shouldn't be multiple matches, but take the last if there are.

        EXPECTED="$inputdir/expected/${name}"
        for LINE in $SUBSTLIST
        do
            if [ `expr "$LINE" : "$name="` -ne 0 ]
            then
                SUBST=`echo "$LINE" | sed 's/^.*=//'`
                EXPECTED="$inputdir/expected/${SUBST}"
            fi
        done

        # If there are multiple equally valid result files, loop to get the right one.
        # If none match, diff against the closest one.

        bestfile=
        bestdiff=
        result=2
        for thisfile in $EXPECTED.out ${EXPECTED}_[0-9].out; do
            [ ! -r "$thisfile" ] && continue
            diff $DIFFFLAGS $thisfile $outputdir/results/${name}.out >/dev/null 2>&1
            result=$?
            case $result in
                0) break;;
                1) thisdiff=`diff $DIFFFLAGS $thisfile $outputdir/results/${name}.out | wc -l`
                   if [ -z "$bestdiff" ] || [ "$thisdiff" -lt "$bestdiff" ]; then
                       bestdiff=$thisdiff; bestfile=$thisfile
                   fi
                   continue;;
                2) break;;
            esac
        done

        # Now print the result.

        case $result in
            0)
                echo "ok";;
            1)
                ( diff $DIFFFLAGS -C3 $bestfile $outputdir/results/${name}.out
                  echo
                  echo "======================================================================"
                  echo ) >> "$diff_file"
                if echo " $ignore_list " | grep " $name " >/dev/null 2>&1 ; then
                    echo "failed (ignored)"
                else
                    echo "FAILED"
                fi
                ;;
            2)
                # disaster struck
                echo "trouble" 1>&2
                (exit 2); exit;;
        esac
    done
done | tee "$result_summary_file" 2>&1

[ $? -ne 0 ] && exit

# ----------
# Server shutdown
# ----------

if [ -n "$postmaster_pid" ]; then
    message "shutting down postmaster"
    "$bindir/pg_ctl" -s -D "$PGDATA" stop
    wait "$postmaster_pid"
    unset postmaster_pid
fi

rm -f "$TMPFILE"


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


(exit $result); exit
