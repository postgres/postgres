#! /bin/sh
# $PostgreSQL: pgsql/src/interfaces/ecpg/test/pg_regress.sh,v 1.1 2006/08/02 13:53:45 meskes Exp $

me=`basename $0`

. pg_regress.inc.sh

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
export ECPG_DONT_LOG_PID=1
export PGPORT=$temp_port
export LD_LIBRARY_PATH=$libdir

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

	runprg=${i/.pgc/}
	outfile_stderr=$outputdir/${runprg//\//-}.stderr
	outfile_stdout=$outputdir/${runprg//\//-}.stdout
	cp $runprg.c "$outputdir/${runprg//\//-}.c"
#	echo "$runprg > $outfile_stdout 2> $outfile_stderr"
	$runprg > "$outfile_stdout" 2> "$outfile_stderr"
	DIFFER=""
	diff -u expected/${runprg//\//-}.stderr "$outputdir"/${runprg//\//-}.stderr >/dev/null 2>&1 || DIFFER="$DIFFER, log"
	diff -u expected/${runprg//\//-}.stdout "$outputdir"/${runprg//\//-}.stdout >/dev/null 2>&1 || DIFFER="$DIFFER, output"
	diff -u expected/${runprg//\//-}.c "$outputdir"/${runprg//\//-}.c >/dev/null 2>&1 || DIFFER="$DIFFER, source"
	DIFFER=${DIFFER#, }
	if [ "x$DIFFER" = "x" ]; then
		echo ok
	else
		echo "FAILED ($DIFFER)"
	fi
done

diff -ur expected/ $outputdir > regression.diff && rm regression.diff

[ $? -ne 0 ] && exit

postmaster_shutdown
evaluate

(exit $result); exit

