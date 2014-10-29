use strict;
use warnings;
use TestLib;
use Test::More tests => 2;

my $tempdir = tempdir;
start_test_server $tempdir;

$ENV{PGOPTIONS} = '--client-min-messages=WARNING';

issues_sql_like(
	[ 'reindexdb', '-a' ],
	qr/statement: REINDEX.*statement: REINDEX/s,
	'reindex all databases');
