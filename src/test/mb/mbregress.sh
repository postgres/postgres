#! /bin/sh
# src/test/mb/mbregress.sh

if echo '\c' | grep -s c >/dev/null 2>&1
then
	ECHO_N="echo -n"
	ECHO_C=""
else
	ECHO_N="echo"
	ECHO_C='\c'
fi

if [ ! -d results ];then
    mkdir results
fi

dropdb --if-exists utf8
createdb -T template0 -l C -E UTF8 utf8 || exit 1

PSQL="psql -n -e -q"
tests="euc_jp sjis euc_kr euc_cn euc_tw big5 utf8 mule_internal"
EXITCODE=0

unset PGCLIENTENCODING
for i in $tests
do
	$ECHO_N "${i} .. " $ECHO_C

	if [ $i = sjis ];then
		PGCLIENTENCODING=SJIS
		export PGCLIENTENCODING
		$PSQL euc_jp < sql/sjis.sql > results/sjis.out 2>&1
		unset PGCLIENTENCODING
        elif [ $i = big5 ];then
		PGCLIENTENCODING=BIG5
		export PGCLIENTENCODING
		$PSQL euc_tw < sql/big5.sql > results/big5.out 2>&1
		unset PGCLIENTENCODING
	else
		dropdb $i >/dev/null 2>&1
		createdb -T template0 -l C -E `echo $i | tr 'abcdefghijklmnopqrstuvwxyz' 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'` $i >/dev/null
		$PSQL $i < sql/${i}.sql > results/${i}.out 2>&1
	fi

	if [ -f expected/${i}-${SYSTEM}.out ]
	then
		EXPECTED="expected/${i}-${SYSTEM}.out"
	else
		EXPECTED="expected/${i}.out"
	fi

	if [ `diff ${EXPECTED} results/${i}.out | wc -l` -ne 0 ]
	then
		( diff -C3 ${EXPECTED} results/${i}.out; \
		echo "";  \
		echo "----------------------"; \
		echo "" ) >> regression.diffs
		echo failed
		EXITCODE=1
	else
		echo ok
	fi
done

exit $EXITCODE
