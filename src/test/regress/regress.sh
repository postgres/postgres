#!/bin/sh
# $Header: /cvsroot/pgsql/src/test/regress/Attic/regress.sh,v 1.51 2000/05/24 22:32:58 tgl Exp $
#
if [ $# -eq 0 ]; then
	echo "Syntax: $0 <hostname> [extra-tests]"
	exit 1
fi

hostname=$1
shift
extratests="$*"

case $hostname in
	i*86-pc-cygwin* | i386-*-qnx*)
 		HOSTLOC="-h localhost"
		;;
 	*)
		HOSTLOC=""
		;;
esac

if echo '\c' | grep -s c >/dev/null 2>&1
then
	ECHO_N="echo -n"
	ECHO_C=""
else
	ECHO_N="echo"
	ECHO_C='\c'
fi

PGTZ="PST8PDT"; export PGTZ
PGDATESTYLE="Postgres,US"; export PGDATESTYLE
LANG= ; export LANG
LC_ALL= ; export LC_ALL

FRONTEND="psql $HOSTLOC -a -q -X"

# ----------
# Scan resultmap file to find which platform-specific expected files to use.
# The format of each line of the file is
#		testname/hostnamepattern=substitutefile
# where the hostnamepattern is evaluated per the rules of expr(1) --- namely,
# it is a standard regular expression with an implicit ^ at the start.
#
# The tempfile hackery is needed because some shells will run the loop
# inside a subshell, whereupon shell variables set therein aren't seen
# outside the loop :-(
# ----------
TMPFILE="matches.$$"
cat /dev/null > $TMPFILE
while read LINE
do
	HOSTPAT=`expr "$LINE" : '.*/\(.*\)='`
	if [ `expr "$hostname" : "$HOSTPAT"` -ne 0 ]
	then
		# remove hostnamepattern from line so that there are no shell
		# wildcards in SUBSTLIST; else later 'for' could expand them!
		TESTNAME=`expr "$LINE" : '\(.*\)/'`
		SUBST=`echo "$LINE" | sed 's/^.*=//'`
		echo "$TESTNAME=$SUBST" >> $TMPFILE
	fi
done <resultmap
SUBSTLIST=`cat $TMPFILE`
rm -f $TMPFILE

if [ -d ./obj ]; then
	cd ./obj
fi

echo "=============== Notes...                              ================="
echo "postmaster must already be running for the regression tests to succeed."
echo "The time zone is set to PST8PDT for these tests by the client frontend."
echo "Please report any apparent problems to ports@postgresql.org"
echo "See regress/README for more information."
echo ""

echo "=============== dropping old regression database...   ================="
dropdb $HOSTLOC regression

echo "=============== creating new regression database...   ================="
if [ -n "$MULTIBYTE" ];then
	mbtests=`echo $MULTIBYTE | tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz'`
	PGCLIENTENCODING="$MULTIBYTE"
	export PGCLIENTENCODING
	ENCODINGOPT="-E $MULTIBYTE"
else
	mbtests=""
	unset PGCLIENTENCODING
	ENCODINGOPT=""
fi
createdb $ENCODINGOPT $HOSTLOC regression
if [ $? -ne 0 ]; then
     echo createdb failed
     exit 1
fi

if [ "x$hostname" != "xi386-pc-qnx" ]; then
echo "=============== installing languages...               ================="
$ECHO_N "installing PL/pgSQL .. " $ECHO_C
createlang $HOSTLOC plpgsql regression
if [ $? -ne 0 -a $? -ne 2 ]; then
     echo failed
     exit 1
else
	echo ok
fi
fi

echo "=============== running regression queries...         ================="
cat /dev/null > regression.diffs

if [ "x$hostname" = "xi386-pc-qnx" ]; then
	DIFFOPT="-b"
else
	DIFFOPT="-w"
fi

stdtests=`awk '
$1=="test"	{ print $2; }
			{}
' < sql/run_check.tests`

for tst in $stdtests $mbtests $extratests
do
	$ECHO_N "${tst} .. " $ECHO_C
	$FRONTEND regression < sql/${tst}.sql > results/${tst}.out 2>&1

	#
	# Check list extracted from resultmap to see if we should compare
	# to a system-specific expected file.
	# There shouldn't be multiple matches, but take the last if there are.
	#
	EXPECTED="expected/${tst}.out"
	for LINE in $SUBSTLIST
	do
		if [ `expr "$LINE" : "$tst="` -ne 0 ]
		then
			SUBST=`echo "$LINE" | sed 's/^.*=//'`
			EXPECTED="expected/${SUBST}.out"
		fi
	done

	if [ `diff ${DIFFOPT} ${EXPECTED} results/${tst}.out | wc -l` -ne 0 ]; then
		( diff ${DIFFOPT} -C3 ${EXPECTED} results/${tst}.out; \
		echo "";  \
		echo "----------------------"; \
		echo "" ) >> regression.diffs
		echo failed
	else
		echo ok
	fi
done

exit 0

echo "=============== running error queries ...             ================="
$FRONTEND regression < errors.sql
# this will generate error result code

#set this to 1 to avoid clearing the database
debug=0

if [ test "$debug" -eq 1 ]; then
	echo Skipping clearing and deletion of the regression database
else
	echo "=============== clearing regression database...       ================="
	$FRONTEND regression < drop.sql
	if [ $? -ne 0 ]; then
		echo the drop script has an error
		exit 1
	fi

	exit 0
	echo "=============== dropping regression database...       ================="
	dropdb regression
	if [ $? -ne 0 ]; then
		echo dropdb failed
		exit 1
	fi

	exit 0
fi
