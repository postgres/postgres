use strict;
use warnings;
use TestLib;
use Test::More tests => 2;

my $tempdir = tempdir;
start_test_server $tempdir;

issues_sql_like(
	[ 'vacuumdb', '-a' ],
	qr/statement: VACUUM.*statement: VACUUM/s,
	'vacuum all databases');
