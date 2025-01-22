
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use RewindTest;

RewindTest::setup_cluster("local");
RewindTest::start_primary();

# Create a test table and insert a row in primary.
primary_psql("CREATE TABLE tbl1 (d text)");
primary_psql("INSERT INTO tbl1 VALUES ('in primary')");
primary_psql("CHECKPOINT");

RewindTest::create_standby("local");

# Insert additional data on primary that will be replicated to standby
primary_psql("INSERT INTO tbl1 values ('in primary, before promotion')");
primary_psql('CHECKPOINT');

RewindTest::promote_standby();

# Insert a row in the old primary. This causes the primary and standby to have
# "diverged", it's no longer possible to just apply the standby's logs over
# primary directory - you need to rewind.  Also insert a new row in the
# standby, which won't be present in the old primary.
primary_psql("INSERT INTO tbl1 VALUES ('in primary, after promotion')");
standby_psql("INSERT INTO tbl1 VALUES ('in standby, after promotion')");

# Stop the nodes before running pg_rewind
$node_standby->stop;
$node_primary->stop;

my $primary_pgdata = $node_primary->data_dir;
my $standby_pgdata = $node_standby->data_dir;

# Add an extra file that we can tamper with without interfering with the data
# directory data files.
mkdir "$standby_pgdata/tst_both_dir";
append_to_file "$standby_pgdata/tst_both_dir/file1", 'a';

# Run pg_rewind and pipe the output from the run into the extra file we want
# to copy. This will ensure that the file is continuously growing during the
# copy operation and the result will be an error.
my $ret = run_log(
	[
		'pg_rewind', '--debug',
		'--source-pgdata' => $standby_pgdata,
		'--target-pgdata' => $primary_pgdata,
		'--no-sync',
	],
	'2>>',
	"$standby_pgdata/tst_both_dir/file1");
ok(!$ret, 'Error out on copying growing file');

# Ensure that the files are of different size, the final error message should
# only be in one of them making them guaranteed to be different
my $primary_size = -s "$primary_pgdata/tst_both_dir/file1";
my $standby_size = -s "$standby_pgdata/tst_both_dir/file1";
isnt($standby_size, $primary_size, "File sizes should differ");

# Extract the last line from the verbose output as that should have the error
# message for the unexpected file size
my $last;
open my $f, '<', "$standby_pgdata/tst_both_dir/file1" or die $!;
$last = $_ while (<$f>);
close $f;
like($last, qr/error: size of source file/, "Check error message");

done_testing();
