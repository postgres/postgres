#!/usr/local/bin/perl -w

# $Id: test.pl,v 1.9 1998/09/27 19:12:26 mergl Exp $

# Before `make install' is performed this script should be runnable with
# `make test'. After `make install' it should work as `perl test.pl'

######################### We start with some black magic to print on failure.

BEGIN { $| = 1; }
END {print "test failed\n" unless $loaded;}
use Pg;
$loaded = 1;
use strict;

######################### End of black magic.

my $dbmain = 'template1';
my $dbname = 'pgperltest';
my $trace  = '/tmp/pgtrace.out';
my ($conn, $result, $i);

my $DEBUG  = 0; # set this to 1 for traces

######################### the following methods will be tested

#	connectdb
#	conndefaults
#	db
#	user
#	port
#	status
#	errorMessage
#	trace
#	untrace
#	exec
#	getline
#	putline
#	endcopy
#	resultStatus
#	fname
#	fnumber
#	ftype
#	fsize
#	cmdStatus
#	oidStatus
#	cmdTuples
#	fetchrow

######################### the following methods will not be tested

#	setdb
#	setdbLogin
#	reset
#	requestCancel
#	pass
#	host
#	tty
#	options
#	socket
#	backendPID
#	notifies
#	sendQuery
#	getResult
#	isBusy
#	consumeInput
#	getlineAsync
#	putnbytes
#	makeEmptyPGresult
#	ntuples
#	nfields
#	binaryTuples
#	fmod
#	getvalue
#	getlength
#	getisnull
#	print
#	displayTuples
#	printTuples
#	lo_import
#	lo_export
#	lo_unlink
#	lo_open
#	lo_close
#	lo_read
#	lo_write
#	lo_creat
#	lo_seek
#	lo_tell

######################### handles error condition

$SIG{PIPE} = sub { print "broken pipe\n" };

######################### create and connect to test database

my $Option_ref = Pg::conndefaults();
my ($key, $val);
( $$Option_ref{port} ne "" && $$Option_ref{dbname} ne "" && $$Option_ref{user} ne "" )
    and print "Pg::conndefaults ........ ok\n"
    or  die   "Pg::conndefaults ........ not ok: ", $conn->errorMessage;

$conn = Pg::connectdb("dbname=$dbmain");
( PGRES_CONNECTION_OK eq $conn->status )
    and print "Pg::connectdb ........... ok\n"
    or  die   "Pg::connectdb ........... not ok: ", $conn->errorMessage;

# do not complain when dropping $dbname
$conn->exec("DROP DATABASE $dbname");

$result = $conn->exec("CREATE DATABASE $dbname");
( PGRES_COMMAND_OK eq $result->resultStatus )
    and print "\$conn->exec ............. ok\n"
    or  die   "\$conn->exec ............. not ok: ", $conn->errorMessage;

$conn = Pg::connectdb("dbname=rumpumpel");
( $conn->errorMessage =~ 'Database rumpumpel does not exist' )
    and print "\$conn->errorMessage ..... ok\n"
    or  die   "\$conn->errorMessage ..... not ok: ", $conn->errorMessage;

$conn = Pg::connectdb("dbname=$dbname");
die $conn->errorMessage unless PGRES_CONNECTION_OK eq $conn->status;

######################### debug, PQtrace

if ($DEBUG) {
    open(FD, ">$trace") || die "can not open $trace: $!";
    $conn->trace("FD");
}

######################### check PGconn

my $db = $conn->db;
( $dbname eq $db )
    and print "\$conn->db ............... ok\n"
    or  print "\$conn->db ............... not ok: $db\n";

my $user = $conn->user;
( "" ne $user )
    and print "\$conn->user ............. ok\n"
    or  print "\$conn->user ............. not ok: $user\n";

my $port = $conn->port;
( "" ne $port )
    and print "\$conn->port ............. ok\n"
    or  print "\$conn->port ............. not ok: $port\n";

######################### create and insert into table

$result = $conn->exec("CREATE TABLE person (id int4, name char(16))");
die $conn->errorMessage unless PGRES_COMMAND_OK eq $result->resultStatus;
my $cmd = $result->cmdStatus;
( "CREATE" eq $cmd )
    and print "\$conn->cmdStatus ........ ok\n"
    or  print "\$conn->cmdStatus ........ not ok: $cmd\n";

for ($i = 1; $i <= 5; $i++) {
    $result = $conn->exec("INSERT INTO person VALUES ($i, 'Edmund Mergl')");
    die $conn->errorMessage unless PGRES_COMMAND_OK eq $result->resultStatus;
}
my $oid = $result->oidStatus;
( 0 != $oid )
    and print "\$conn->oidStatus ........ ok\n"
    or  print "\$conn->oidStatus ........ not ok: $oid\n";

######################### copy to stdout, PQgetline

$result = $conn->exec("COPY person TO STDOUT");
die $conn->errorMessage unless PGRES_COPY_OUT eq $result->resultStatus;

my $ret = 0;
my $buf;
my $string;
$i = 1;
while (-1 != $ret) {
    $ret = $conn->getline($buf, 256);
    last if $buf eq "\\.";
    $string = $buf if 1 == $i;
    $i++;
}
( "1	Edmund Mergl    " eq $string )
    and print "\$conn->getline .......... ok\n"
    or  print "\$conn->getline .......... not ok: $string\n";

$ret = $conn->endcopy;
( 0 == $ret )
    and print "\$conn->endcopy .......... ok\n"
    or  print "\$conn->endcopy .......... not ok: $ret\n";

######################### delete and copy from stdin, PQputline

$result = $conn->exec("BEGIN");
die $conn->errorMessage unless PGRES_COMMAND_OK eq $result->resultStatus;

$result = $conn->exec("DELETE FROM person");
die $conn->errorMessage unless PGRES_COMMAND_OK eq $result->resultStatus;
$ret = $result->cmdTuples;
( 5 == $ret )
    and print "\$result->cmdTuples ...... ok\n"
    or  print "\$result->cmdTuples ...... not ok: $ret\n";

$result = $conn->exec("COPY person FROM STDIN");
die $conn->errorMessage unless PGRES_COPY_IN eq $result->resultStatus;

for ($i = 1; $i <= 5; $i++) {
    # watch the tabs and do not forget the newlines
    $conn->putline("$i	Edmund Mergl\n");
}
$conn->putline("\\.\n");

die $conn->errorMessage if $conn->endcopy;

$result = $conn->exec("END");
die $conn->errorMessage unless PGRES_COMMAND_OK eq $result->resultStatus;

######################### select from person, PQgetvalue

$result = $conn->exec("SELECT * FROM person");
die $conn->errorMessage unless PGRES_TUPLES_OK eq $result->resultStatus;

my $fname = $result->fname(0);
( "id" eq $fname )
    and print "\$result->fname .......... ok\n"
    or  print "\$result->fname .......... not ok: $fname\n";

my $ftype = $result->ftype(0);
( 23 == $ftype )
    and print "\$result->ftype .......... ok\n"
    or  print "\$result->ftype .......... not ok: $ftype\n";

my $fsize = $result->fsize(0);
( 4 == $fsize )
    and print "\$result->fsize .......... ok\n"
    or  print "\$result->fsize .......... not ok: $fsize\n";

my $fnumber = $result->fnumber($fname);
( 0 == $fnumber )
    and print "\$result->fnumber ........ ok\n"
    or  print "\$result->fnumber ........ not ok: $fnumber\n";

$string = "";
my @row;
while (@row = $result->fetchrow) {
    $string = join(" ", @row);
}
( "5 Edmund Mergl    " eq $string )
    and print "\$result->fetchrow ....... ok\n"
    or  print "\$result->fetchrow ....... not ok: $string\n";

######################### debug, PQuntrace

if ($DEBUG) {
    close(FD) || die "bad TRACE: $!";
    $conn->untrace;
}

######################### disconnect and drop test database

$conn = Pg::connectdb("dbname=$dbmain");
die $conn->errorMessage unless PGRES_CONNECTION_OK eq $conn->status;

$result = $conn->exec("DROP DATABASE $dbname");
die $conn->errorMessage unless PGRES_COMMAND_OK eq $result->resultStatus;

print "test sequence finished.\n";

######################### EOF
