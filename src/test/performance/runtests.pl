#!/usr/bin/perl
#
# Accepts one argument - DBMS name (pgsql, ...) and initializes
# global variable $TestDBMS with this name.
#

# Where to run tests
$DBNAME = 'perftest';

# This describtion for all DBMS supported by test
# DBMS_name => [FrontEnd, DestroyDB command, CreateDB command]

%DBMS =
  ('pgsql' =>
	  [ "psql -q -d $DBNAME", "destroydb $DBNAME", "createdb $DBNAME" ]);

# Tests to run: test' script, test' description, ...
# Test' script is in form
#
# 	script_name[.ntm][ T]
#
# script_name is name of file in ./sqls
# .ntm means that script will be used for some initialization
#      and should not be timed: runtests.pl opens /dev/null as STDERR
#      in this case and restore STDERR to result file after script done.
#      Script shouldn't notice either he is running for test or for
#      initialization purposes.
# T means that all queries in this test (initialization ?) are to be
# executed in SINGLE transaction. In this case global variable $XACTBLOCK
# is not empty string. Otherwise, each query in test is to be executed
# in own transaction ($XACTBLOCK is empty string). In accordance with
# $XACTBLOCK, script is to do DBMS specific preparation before execution
# of queries. (Look at example in sqls/inssimple for MySQL - it gives
# an idea of what can be done for features unsupported by an DBMS.)
#
@perftests = (

	# It speed up things
	'connection.ntm', 'DB connection startup (no timing)',

	# Just connection startup time (echo "" | psql ... - for PgSQL)
	'connection',    'DB connection startup',
	'crtsimple.ntm', 'Create SIMPLE table (no timing)',

	# 8192 inserts in single xaction
	'inssimple T',   '8192 INSERTs INTO SIMPLE (1 xact)',
	'drpsimple.ntm', 'Drop SIMPLE table (no timing)',
	'crtsimple.ntm', 'Create SIMPLE table (no timing)',

	# 8192 inserts in 8192 xactions
	'inssimple',  '8192 INSERTs INTO SIMPLE (8192 xacts)',
	'vacuum.ntm', 'Vacuum (no timing)',

	# Fast (after table filled with data) index creation test
	'crtsimpleidx',     'Create INDEX on SIMPLE',
	'drpsimple.ntm',    'Drop SIMPLE table (no timing)',
	'crtsimple.ntm',    'Create SIMPLE table (no timing)',
	'crtsimpleidx.ntm', 'Create INDEX on SIMPLE (no timing)',

	# 8192 inserts in single xaction into table with index
	'inssimple T', '8192 INSERTs INTO SIMPLE with INDEX (1 xact)',

	# 8192 SELECT * FROM simple WHERE justint = <random_key> in single xaction
	'slcsimple T', '8192 random INDEX scans on SIMPLE (1 xact)',

	# SELECT * FROM simple ORDER BY justint
	'orbsimple', 'ORDER BY SIMPLE',);

#
# It seems that nothing below need to be changed
#

$TestDBMS = $ARGV[0];
die "Unsupported DBMS $TestDBMS\n" if !exists $DBMS{$TestDBMS};

$FrontEnd  = $DBMS{$TestDBMS}[0];
$DestroyDB = $DBMS{$TestDBMS}[1];
$CreateDB  = $DBMS{$TestDBMS}[2];

print "(Re)create DataBase $DBNAME\n";

`$DestroyDB`;    # Destroy DB
`$CreateDB`;     # Create DB

$ResFile = "Results.$TestDBMS";
$TmpFile = "Tmp.$TestDBMS";

open(SAVEOUT, ">&STDOUT");
open(STDOUT,  ">/dev/null") or die;
open(SAVEERR, ">&STDERR");
open(STDERR,  ">$TmpFile") or die;
select(STDERR);
$| = 1;

for ($i = 0; $i <= $#perftests; $i++)
{
	$test = $perftests[$i];
	($test, $XACTBLOCK) = split(/ /, $test);
	$runtest = $test;
	if ($test =~ /\.ntm/)
	{

		#
		# No timing for this queries
		#
		close(STDERR);    # close $TmpFile
		open(STDERR, ">/dev/null") or die;
		$runtest =~ s/\.ntm//;
	}
	else
	{
		close(STDOUT);
		open(STDOUT, ">&SAVEOUT");
		print STDOUT "\nRunning: $perftests[$i+1] ...";
		close(STDOUT);
		open(STDOUT, ">/dev/null") or die;
		select(STDERR);
		$| = 1;
		printf "$perftests[$i+1]: ";
	}

	do "sqls/$runtest";

	# Restore STDERR to $TmpFile
	if ($test =~ /\.ntm/)
	{
		close(STDERR);
		open(STDERR, ">>$TmpFile") or die;
	}

	select(STDERR);
	$| = 1;
	$i++;
}

close(STDERR);
open(STDERR, ">&SAVEERR");

open(TMPF, "<$TmpFile") or die;
open(RESF, ">$ResFile") or die;

while (<TMPF>)
{
	$str = $_;
	($test, $rtime) = split(/:/, $str);
	($tmp, $rtime, $rest) = split(/[ 	]+/, $rtime);
	print RESF "$test: $rtime\n";
}
