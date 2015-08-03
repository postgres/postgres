use strict;
use warnings;
use TestLib;
use Test::More tests => 8;

use RewindTest;

sub run_test
{
	my $test_mode = shift;

	RewindTest::setup_cluster();
	RewindTest::start_master();

	# Create a test table and insert a row in master.
	master_psql("CREATE TABLE tbl1 (d text)");
	master_psql("INSERT INTO tbl1 VALUES ('in master')");

	# This test table will be used to test truncation, i.e. the table
	# is extended in the old master after promotion
	master_psql("CREATE TABLE trunc_tbl (d text)");
	master_psql("INSERT INTO trunc_tbl VALUES ('in master')");

	# This test table will be used to test the "copy-tail" case, i.e. the
	# table is truncated in the old master after promotion
	master_psql("CREATE TABLE tail_tbl (id integer, d text)");
	master_psql("INSERT INTO tail_tbl VALUES (0, 'in master')");

	master_psql("CHECKPOINT");

	RewindTest::create_standby();

	# Insert additional data on master that will be replicated to standby
	master_psql("INSERT INTO tbl1 values ('in master, before promotion')");
	master_psql(
		"INSERT INTO trunc_tbl values ('in master, before promotion')");
	master_psql(
"INSERT INTO tail_tbl SELECT g, 'in master, before promotion: ' || g FROM generate_series(1, 10000) g"
	);

	master_psql('CHECKPOINT');

	RewindTest::promote_standby();

	# Insert a row in the old master. This causes the master and standby
	# to have "diverged", it's no longer possible to just apply the
	# standy's logs over master directory - you need to rewind.
	master_psql("INSERT INTO tbl1 VALUES ('in master, after promotion')");

	# Also insert a new row in the standby, which won't be present in the
	# old master.
	standby_psql("INSERT INTO tbl1 VALUES ('in standby, after promotion')");

	# Insert enough rows to trunc_tbl to extend the file. pg_rewind should
	# truncate it back to the old size.
	master_psql(
"INSERT INTO trunc_tbl SELECT 'in master, after promotion: ' || g FROM generate_series(1, 10000) g"
	);

	# Truncate tail_tbl. pg_rewind should copy back the truncated part
	# (We cannot use an actual TRUNCATE command here, as that creates a
	# whole new relfilenode)
	master_psql("DELETE FROM tail_tbl WHERE id > 10");
	master_psql("VACUUM tail_tbl");

	RewindTest::run_pg_rewind($test_mode);

	check_query(
		'SELECT * FROM tbl1',
		qq(in master
in master, before promotion
in standby, after promotion
),
		'table content');

	check_query(
		'SELECT * FROM trunc_tbl',
		qq(in master
in master, before promotion
),
		'truncation');

	check_query(
		'SELECT count(*) FROM tail_tbl',
		qq(10001
),
		'tail-copy');

	RewindTest::clean_rewind_test();
}

# Run the test in both modes
run_test('local');
run_test('remote');

exit(0);
