#! /bin/sh
# $PostgreSQL: pgsql/src/interfaces/ecpg/test/pg_regress.sh,v 1.5 2006/08/08 11:51:24 meskes Exp $

me=`basename $0`

. ./pg_regress.inc.sh

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
else # not temp-install
	dont_temp_install
fi

# ----------
# Postmaster is started, now we can change some environment variables for the
# client
# ----------

setup_client_environment_variables

# set up the dbs we use for ecpg regression tests
#"$bindir/createdb" $encoding_opt $psql_options --template template0 regress1
#"$bindir/createdb" $encoding_opt $psql_options --template template0 connectdb
#database_cleanup
create_database $dbname
create_database connectdb

# ----------
# Let's go
# ----------

message "running regression test queries"

outputdir="results/"

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

# this variable prevents that the PID gets included in the logfiles
ECPG_REGRESSION=1; export ECPG_REGRESSION
PGPORT=$temp_port; export PGPORT
LD_LIBRARY_PATH=$libdir:$LD_LIBRARY_PATH; export LD_LIBRARY_PATH

DIFFFLAGS="$DIFFFLAGS -C3"
FAILNUM=""

rm -f regression.diffs

for i in \
         connect/*.pgc \
         compat_informix/*.pgc \
         complex/*.pgc \
         errors/*.pgc \
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
			cat $f.tmp | sed -e s,$PGPORT,55432,g >> $f
			rm $f.tmp
		done
	fi

	DIFFER=""
	diff $DIFFFLAGS expected/$outprg.stderr "$outfile_stderr" >> regression.diffs 2>&1 || DIFFER="$DIFFER, log"
	diff $DIFFFLAGS expected/$outprg.stdout "$outfile_stdout" >> regression.diffs 2>&1 || DIFFER="$DIFFER, output"
	diff $DIFFFLAGS expected/$outprg.c "$outputdir"/$outprg.c >> regression.diffs 2>&1 || DIFFER="$DIFFER, source"

	DIFFER=`echo $DIFFER | sed -e 's/^, //'`
	if [ "x$DIFFER" = "x" ]; then
		echo ok
	else
		echo "FAILED ($DIFFER)"
		# some sh's don't know about $((x+1))
		FAILNUM=x$FAILNUM
	fi
done

if [ "x$FAILNUM" = x"" ]; then
	rm regression.diffs
fi

postmaster_shutdown

# FAILNUM is empty if no test has failed
[ x"$FAILNUM" = x"" ] && exit 0
(exit 1); exit

