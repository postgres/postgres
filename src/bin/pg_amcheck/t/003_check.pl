
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my ($node, $port, %corrupt_page, %remove_relation);

# Returns the filesystem path for the named relation.
#
# Assumes the test node is running
sub relation_filepath
{
	my ($dbname, $relname) = @_;

	my $pgdata = $node->data_dir;
	my $rel =
	  $node->safe_psql($dbname, qq(SELECT pg_relation_filepath('$relname')));
	die "path not found for relation $relname" unless defined $rel;
	return "$pgdata/$rel";
}

# Returns the name of the toast relation associated with the named relation.
#
# Assumes the test node is running
sub relation_toast
{
	my ($dbname, $relname) = @_;

	my $rel = $node->safe_psql(
		$dbname, qq(
		SELECT c.reltoastrelid::regclass
			FROM pg_catalog.pg_class c
			WHERE c.oid = '$relname'::regclass
			  AND c.reltoastrelid != 0
			));
	return $rel;
}

# Adds the relation file for the given (dbname, relname) to the list
# to be corrupted by means of overwriting junk in the first page.
#
# Assumes the test node is running.
sub plan_to_corrupt_first_page
{
	my ($dbname, $relname) = @_;
	my $relpath = relation_filepath($dbname, $relname);
	$corrupt_page{$relpath} = 1;
}

# Adds the relation file for the given (dbname, relname) to the list
# to be corrupted by means of removing the file..
#
# Assumes the test node is running
sub plan_to_remove_relation_file
{
	my ($dbname, $relname) = @_;
	my $relpath = relation_filepath($dbname, $relname);
	$remove_relation{$relpath} = 1;
}

# For the given (dbname, relname), if a corresponding toast table
# exists, adds that toast table's relation file to the list to be
# corrupted by means of removing the file.
#
# Assumes the test node is running.
sub plan_to_remove_toast_file
{
	my ($dbname, $relname) = @_;
	my $toastname = relation_toast($dbname, $relname);
	plan_to_remove_relation_file($dbname, $toastname) if ($toastname);
}

# Corrupts the first page of the given file path
sub corrupt_first_page
{
	my ($relpath) = @_;

	my $fh;
	open($fh, '+<', $relpath)
	  or BAIL_OUT("open failed: $!");
	binmode $fh;

	# Corrupt some line pointers.  The values are chosen to hit the
	# various line-pointer-corruption checks in verify_heapam.c
	# on both little-endian and big-endian architectures.
	sysseek($fh, 32, 0)
	  or BAIL_OUT("sysseek failed: $!");
	syswrite(
		$fh,
		pack("L*",
			0xAAA15550, 0xAAA0D550, 0x00010000, 0x00008000,
			0x0000800F, 0x001e8000, 0xFFFFFFFF)
	) or BAIL_OUT("syswrite failed: $!");
	close($fh)
	  or BAIL_OUT("close failed: $!");
}

# Stops the node, performs all the corruptions previously planned, and
# starts the node again.
#
sub perform_all_corruptions()
{
	$node->stop();
	for my $relpath (keys %corrupt_page)
	{
		corrupt_first_page($relpath);
	}
	for my $relpath (keys %remove_relation)
	{
		unlink($relpath);
	}
	$node->start;
}

# Test set-up
$node = PostgreSQL::Test::Cluster->new('test');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum=off');
$node->start;
$port = $node->port;

for my $dbname (qw(db1 db2 db3))
{
	# Create the database
	$node->safe_psql('postgres', qq(CREATE DATABASE $dbname));

	# Load the amcheck extension, upon which pg_amcheck depends.  Put the
	# extension in an unexpected location to test that pg_amcheck finds it
	# correctly.  Create tables with names that look like pg_catalog names to
	# check that pg_amcheck does not get confused by them.  Create functions in
	# schema public that look like amcheck functions to check that pg_amcheck
	# does not use them.
	$node->safe_psql(
		$dbname, q(
		CREATE SCHEMA amcheck_schema;
		CREATE EXTENSION amcheck WITH SCHEMA amcheck_schema;
		CREATE TABLE amcheck_schema.pg_database (junk text);
		CREATE TABLE amcheck_schema.pg_namespace (junk text);
		CREATE TABLE amcheck_schema.pg_class (junk text);
		CREATE TABLE amcheck_schema.pg_operator (junk text);
		CREATE TABLE amcheck_schema.pg_proc (junk text);
		CREATE TABLE amcheck_schema.pg_tablespace (junk text);

		CREATE FUNCTION public.bt_index_check(index regclass,
											  heapallindexed boolean default false)
		RETURNS VOID AS $$
		BEGIN
			RAISE EXCEPTION 'Invoked wrong bt_index_check!';
		END;
		$$ LANGUAGE plpgsql;

		CREATE FUNCTION public.bt_index_parent_check(index regclass,
													 heapallindexed boolean default false,
													 rootdescend boolean default false)
		RETURNS VOID AS $$
		BEGIN
			RAISE EXCEPTION 'Invoked wrong bt_index_parent_check!';
		END;
		$$ LANGUAGE plpgsql;

		CREATE FUNCTION public.verify_heapam(relation regclass,
											 on_error_stop boolean default false,
											 check_toast boolean default false,
											 skip text default 'none',
											 startblock bigint default null,
											 endblock bigint default null,
											 blkno OUT bigint,
											 offnum OUT integer,
											 attnum OUT integer,
											 msg OUT text)
		RETURNS SETOF record AS $$
		BEGIN
			RAISE EXCEPTION 'Invoked wrong verify_heapam!';
		END;
		$$ LANGUAGE plpgsql;
	));

	# Create schemas, tables and indexes in five separate
	# schemas.  The schemas are all identical to start, but
	# we will corrupt them differently later.
	#
	for my $schema (qw(s1 s2 s3 s4 s5))
	{
		$node->safe_psql(
			$dbname, qq(
			CREATE SCHEMA $schema;
			CREATE SEQUENCE $schema.seq1;
			CREATE SEQUENCE $schema.seq2;
			CREATE TABLE $schema.t1 (
				i INTEGER,
				b BOX,
				ia int4[],
				ir int4range,
				t TEXT
			);
			CREATE TABLE $schema.t2 (
				i INTEGER,
				b BOX,
				ia int4[],
				ir int4range,
				t TEXT
			);
			CREATE VIEW $schema.t2_view AS (
				SELECT i*2, t FROM $schema.t2
			);
			ALTER TABLE $schema.t2
				ALTER COLUMN t
				SET STORAGE EXTERNAL;

			INSERT INTO $schema.t1 (i, b, ia, ir, t)
				(SELECT gs::INTEGER AS i,
						box(point(gs,gs+5),point(gs*2,gs*3)) AS b,
						array[gs, gs + 1]::int4[] AS ia,
						int4range(gs, gs+100) AS ir,
						repeat('foo', gs) AS t
					 FROM generate_series(1,10000,3000) AS gs);

			INSERT INTO $schema.t2 (i, b, ia, ir, t)
				(SELECT gs::INTEGER AS i,
						box(point(gs,gs+5),point(gs*2,gs*3)) AS b,
						array[gs, gs + 1]::int4[] AS ia,
						int4range(gs, gs+100) AS ir,
						repeat('foo', gs) AS t
					 FROM generate_series(1,10000,3000) AS gs);

			CREATE MATERIALIZED VIEW $schema.t1_mv AS SELECT * FROM $schema.t1;
			CREATE MATERIALIZED VIEW $schema.t2_mv AS SELECT * FROM $schema.t2;

			create table $schema.p1 (a int, b int) PARTITION BY list (a);
			create table $schema.p2 (a int, b int) PARTITION BY list (a);

			create table $schema.p1_1 partition of $schema.p1 for values in (1, 2, 3);
			create table $schema.p1_2 partition of $schema.p1 for values in (4, 5, 6);
			create table $schema.p2_1 partition of $schema.p2 for values in (1, 2, 3);
			create table $schema.p2_2 partition of $schema.p2 for values in (4, 5, 6);

			CREATE INDEX t1_btree ON $schema.t1 USING BTREE (i);
			CREATE INDEX t2_btree ON $schema.t2 USING BTREE (i);

			CREATE INDEX t1_hash ON $schema.t1 USING HASH (i);
			CREATE INDEX t2_hash ON $schema.t2 USING HASH (i);

			CREATE INDEX t1_brin ON $schema.t1 USING BRIN (i);
			CREATE INDEX t2_brin ON $schema.t2 USING BRIN (i);

			CREATE INDEX t1_gist ON $schema.t1 USING GIST (b);
			CREATE INDEX t2_gist ON $schema.t2 USING GIST (b);

			CREATE INDEX t1_gin ON $schema.t1 USING GIN (ia);
			CREATE INDEX t2_gin ON $schema.t2 USING GIN (ia);

			CREATE INDEX t1_spgist ON $schema.t1 USING SPGIST (ir);
			CREATE INDEX t2_spgist ON $schema.t2 USING SPGIST (ir);

			CREATE UNIQUE INDEX t1_btree_unique ON $schema.t1 USING BTREE (i);
			CREATE UNIQUE INDEX t2_btree_unique ON $schema.t2 USING BTREE (i);
		));
	}
}

# Database 'db1' corruptions
#

# Corrupt indexes in schema "s1"
plan_to_remove_relation_file('db1', 's1.t1_btree');
plan_to_corrupt_first_page('db1', 's1.t2_btree');

# Corrupt tables in schema "s2"
plan_to_remove_relation_file('db1', 's2.t1');
plan_to_corrupt_first_page('db1', 's2.t2');

# Corrupt tables, partitions, matviews, and btrees in schema "s3"
plan_to_remove_relation_file('db1', 's3.t1');
plan_to_corrupt_first_page('db1', 's3.t2');

plan_to_remove_relation_file('db1', 's3.t1_mv');
plan_to_remove_relation_file('db1', 's3.p1_1');

plan_to_corrupt_first_page('db1', 's3.t2_mv');
plan_to_corrupt_first_page('db1', 's3.p2_1');

plan_to_remove_relation_file('db1', 's3.t1_btree');
plan_to_corrupt_first_page('db1', 's3.t2_btree');

# Corrupt toast table, partitions, and materialized views in schema "s4"
plan_to_remove_toast_file('db1', 's4.t2');

# Corrupt all other object types in schema "s5".  We don't have amcheck support
# for these types, but we check that their corruption does not trigger any
# errors in pg_amcheck
plan_to_remove_relation_file('db1', 's5.seq1');
plan_to_remove_relation_file('db1', 's5.t1_hash');
plan_to_remove_relation_file('db1', 's5.t1_gist');
plan_to_remove_relation_file('db1', 's5.t1_gin');
plan_to_remove_relation_file('db1', 's5.t1_brin');
plan_to_remove_relation_file('db1', 's5.t1_spgist');

plan_to_corrupt_first_page('db1', 's5.seq2');
plan_to_corrupt_first_page('db1', 's5.t2_hash');
plan_to_corrupt_first_page('db1', 's5.t2_gist');
plan_to_corrupt_first_page('db1', 's5.t2_gin');
plan_to_corrupt_first_page('db1', 's5.t2_brin');
plan_to_corrupt_first_page('db1', 's5.t2_spgist');


# Database 'db2' corruptions
#
plan_to_remove_relation_file('db2', 's1.t1');
plan_to_remove_relation_file('db2', 's1.t1_btree');


# Leave 'db3' uncorrupted
#

# Standard first arguments to PostgreSQL::Test::Utils functions
my @cmd = ('pg_amcheck', '-p', $port);

# Regular expressions to match various expected output
my $no_output_re = qr/^$/;
my $line_pointer_corruption_re = qr/line pointer/;
my $missing_file_re = qr/could not open file ".*": No such file or directory/;
my $index_missing_relation_fork_re =
  qr/index ".*" lacks a main relation fork/;

# We have created test databases with tables populated with data, but have not
# yet corrupted anything.  As such, we expect no corruption and verify that
# none is reported
#
$node->command_checks_all([ @cmd, '-d', 'db1', '-d', 'db2', '-d', 'db3' ],
	0, [$no_output_re], [$no_output_re], 'pg_amcheck prior to corruption');

# Perform the corruptions we planned above using only a single database restart.
#
perform_all_corruptions();


# Checking databases with amcheck installed and corrupt relations, pg_amcheck
# command itself should return exit status = 2, because tables and indexes are
# corrupt, not exit status = 1, which would mean the pg_amcheck command itself
# failed.  Corruption messages should go to stdout, and nothing to stderr.
#
$node->command_checks_all(
	[ @cmd, 'db1' ],
	2,
	[
		$index_missing_relation_fork_re, $line_pointer_corruption_re,
		$missing_file_re,
	],
	[$no_output_re],
	'pg_amcheck all schemas, tables and indexes in database db1');

$node->command_checks_all(
	[ @cmd, '-d', 'db1', '-d', 'db2', '-d', 'db3' ],
	2,
	[
		$index_missing_relation_fork_re, $line_pointer_corruption_re,
		$missing_file_re,
	],
	[$no_output_re],
	'pg_amcheck all schemas, tables and indexes in databases db1, db2, and db3'
);

# Scans of indexes in s1 should detect the specific corruption that we created
# above.  For missing relation forks, we know what the error message looks
# like.  For corrupted index pages, the error might vary depending on how the
# page was formatted on disk, including variations due to alignment differences
# between platforms, so we accept any non-empty error message.
#
# If we don't limit the check to databases with amcheck installed, we expect
# complaint on stderr, but otherwise stderr should be quiet.
#
$node->command_checks_all(
	[ @cmd, '--all', '-s', 's1', '-i', 't1_btree' ],
	2,
	[$index_missing_relation_fork_re],
	[
		qr/pg_amcheck: warning: skipping database "postgres": amcheck is not installed/
	],
	'pg_amcheck index s1.t1_btree reports missing main relation fork');

$node->command_checks_all(
	[ @cmd, '-d', 'db1', '-s', 's1', '-i', 't2_btree' ],
	2,
	[qr/.+/],    # Any non-empty error message is acceptable
	[$no_output_re],
	'pg_amcheck index s1.s2 reports index corruption');

# Checking db1.s1 with indexes excluded should show no corruptions because we
# did not corrupt any tables in db1.s1.  Verify that both stdout and stderr
# are quiet.
#
$node->command_checks_all(
	[ @cmd, '-t', 's1.*', '--no-dependent-indexes', 'db1' ],
	0, [$no_output_re], [$no_output_re],
	'pg_amcheck of db1.s1 excluding indexes');

# Checking db2.s1 should show table corruptions if indexes are excluded
#
$node->command_checks_all(
	[ @cmd, '-t', 's1.*', '--no-dependent-indexes', 'db2' ],
	2, [$missing_file_re], [$no_output_re],
	'pg_amcheck of db2.s1 excluding indexes');

# In schema db1.s3, the tables and indexes are both corrupt.  We should see
# corruption messages on stdout, and nothing on stderr.
#
$node->command_checks_all(
	[ @cmd, '-s', 's3', 'db1' ],
	2,
	[
		$index_missing_relation_fork_re, $line_pointer_corruption_re,
		$missing_file_re,
	],
	[$no_output_re],
	'pg_amcheck schema s3 reports table and index errors');

# In schema db1.s4, only toast tables are corrupt.  Check that under default
# options the toast corruption is reported, but when excluding toast we get no
# error reports.
$node->command_checks_all([ @cmd, '-s', 's4', 'db1' ],
	2, [$missing_file_re], [$no_output_re],
	'pg_amcheck in schema s4 reports toast corruption');

$node->command_checks_all(
	[
		@cmd, '--no-dependent-toast', '--exclude-toast-pointers', '-s', 's4',
		'db1'
	],
	0,
	[$no_output_re],
	[$no_output_re],
	'pg_amcheck in schema s4 excluding toast reports no corruption');

# Check that no corruption is reported in schema db1.s5
$node->command_checks_all([ @cmd, '-s', 's5', 'db1' ],
	0, [$no_output_re], [$no_output_re],
	'pg_amcheck over schema s5 reports no corruption');

# In schema db1.s1, only indexes are corrupt.  Verify that when we exclude
# the indexes, no corruption is reported about the schema.
#
$node->command_checks_all(
	[ @cmd, '-s', 's1', '-I', 't1_btree', '-I', 't2_btree', 'db1' ],
	0,
	[$no_output_re],
	[$no_output_re],
	'pg_amcheck over schema s1 with corrupt indexes excluded reports no corruption'
);

# In schema db1.s1, only indexes are corrupt.  Verify that when we provide only
# table inclusions, and disable index expansion, no corruption is reported
# about the schema.
#
$node->command_checks_all(
	[ @cmd, '-t', 's1.*', '--no-dependent-indexes', 'db1' ],
	0,
	[$no_output_re],
	[$no_output_re],
	'pg_amcheck over schema s1 with all indexes excluded reports no corruption'
);

# In schema db1.s2, only tables are corrupt.  Verify that when we exclude those
# tables that no corruption is reported.
#
$node->command_checks_all(
	[ @cmd, '-s', 's2', '-T', 't1', '-T', 't2', 'db1' ],
	0,
	[$no_output_re],
	[$no_output_re],
	'pg_amcheck over schema s2 with corrupt tables excluded reports no corruption'
);

# Check errors about bad block range command line arguments.  We use schema s5
# to avoid getting messages about corrupt tables or indexes.
#
command_fails_like(
	[ @cmd, '-s', 's5', '--startblock', 'junk', 'db1' ],
	qr/invalid start block/,
	'pg_amcheck rejects garbage startblock');

command_fails_like(
	[ @cmd, '-s', 's5', '--endblock', '1234junk', 'db1' ],
	qr/invalid end block/,
	'pg_amcheck rejects garbage endblock');

command_fails_like(
	[ @cmd, '-s', 's5', '--startblock', '5', '--endblock', '4', 'db1' ],
	qr/end block precedes start block/,
	'pg_amcheck rejects invalid block range');

# Check bt_index_parent_check alternates.  We don't create any index corruption
# that would behave differently under these modes, so just smoke test that the
# arguments are handled sensibly.
#
$node->command_checks_all(
	[ @cmd, '-s', 's1', '-i', 't1_btree', '--parent-check', 'db1' ],
	2,
	[$index_missing_relation_fork_re],
	[$no_output_re],
	'pg_amcheck smoke test --parent-check');

$node->command_checks_all(
	[
		@cmd, '-s', 's1', '-i', 't1_btree', '--heapallindexed',
		'--rootdescend', 'db1'
	],
	2,
	[$index_missing_relation_fork_re],
	[$no_output_re],
	'pg_amcheck smoke test --heapallindexed --rootdescend');

$node->command_checks_all(
	[ @cmd, '-d', 'db1', '-d', 'db2', '-d', 'db3', '-S', 's*' ],
	0, [$no_output_re], [$no_output_re],
	'pg_amcheck excluding all corrupt schemas');

$node->command_checks_all(
	[
		@cmd, '-s', 's1', '-i', 't1_btree', '--parent-check',
		'--checkunique', 'db1'
	],
	2,
	[$index_missing_relation_fork_re],
	[$no_output_re],
	'pg_amcheck smoke test --parent-check --checkunique');

$node->command_checks_all(
	[
		@cmd, '-s', 's1', '-i', 't1_btree', '--heapallindexed',
		'--rootdescend', '--checkunique', 'db1'
	],
	2,
	[$index_missing_relation_fork_re],
	[$no_output_re],
	'pg_amcheck smoke test --heapallindexed --rootdescend --checkunique');

$node->command_checks_all(
	[
		@cmd, '--checkunique', '-d', 'db1', '-d', 'db2',
		'-d', 'db3', '-S', 's*'
	],
	0,
	[$no_output_re],
	[$no_output_re],
	'pg_amcheck excluding all corrupt schemas with --checkunique option');

#
# Smoke test for checkunique option for not supported versions.
#
$node->safe_psql(
	'db3', q(
		DROP EXTENSION amcheck;
		CREATE EXTENSION amcheck WITH SCHEMA amcheck_schema VERSION '1.3' ;
));

$node->command_checks_all(
	[ @cmd, '--checkunique', 'db3' ],
	0,
	[$no_output_re],
	[
		qr/pg_amcheck: warning: option --checkunique is not supported by amcheck version 1.3/
	],
	'pg_amcheck smoke test --checkunique');
done_testing();
