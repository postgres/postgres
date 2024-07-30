# Copyright (c) 2023-2024, PostgreSQL Global Development Group

# Test wraparound emergency autovacuum.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bxid_wraparound\b/)
{
	plan skip_all => "test xid_wraparound not enabled in PG_TEST_EXTRA";
}

# Initialize node
my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->append_conf(
	'postgresql.conf', qq[
autovacuum_naptime = 1s
# so it's easier to verify the order of operations
autovacuum_max_workers = 1
log_autovacuum_min_duration = 0
]);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION xid_wraparound');

# Create tables for a few different test scenarios. We disable autovacuum
# on these tables to run it only to prevent wraparound.
$node->safe_psql(
	'postgres', qq[
CREATE TABLE large(id serial primary key, data text, filler text default repeat(random()::text, 10))
   WITH (autovacuum_enabled = off);
INSERT INTO large(data) SELECT generate_series(1,30000);

CREATE TABLE large_trunc(id serial primary key, data text, filler text default repeat(random()::text, 10))
   WITH (autovacuum_enabled = off);
INSERT INTO large_trunc(data) SELECT generate_series(1,30000);

CREATE TABLE small(id serial primary key, data text, filler text default repeat(random()::text, 10))
   WITH (autovacuum_enabled = off);
INSERT INTO small(data) SELECT generate_series(1,15000);

CREATE TABLE small_trunc(id serial primary key, data text, filler text default repeat(random()::text, 10))
   WITH (autovacuum_enabled = off);
INSERT INTO small_trunc(data) SELECT generate_series(1,15000);
]);

# Bump the query timeout to avoid false negatives on slow test systems.
my $psql_timeout_secs = 4 * $PostgreSQL::Test::Utils::timeout_default;

# Start a background session, which holds a transaction open, preventing
# autovacuum from advancing relfrozenxid and datfrozenxid.
my $background_psql = $node->background_psql(
	'postgres',
	on_error_stop => 0,
	timeout => $psql_timeout_secs);
$background_psql->set_query_timer_restart();
$background_psql->query_safe(
	qq[
	BEGIN;
	DELETE FROM large WHERE id % 2 = 0;
	DELETE FROM large_trunc WHERE id > 10000;
	DELETE FROM small WHERE id % 2 = 0;
	DELETE FROM small_trunc WHERE id > 1000;
]);

# Consume 2 billion XIDs, to get us very close to wraparound
$node->safe_psql('postgres',
	qq[SELECT consume_xids_until('2000000000'::xid8)]);

# Make sure the latest completed XID is advanced
$node->safe_psql('postgres', qq[INSERT INTO small(data) SELECT 1]);

# Check that all databases became old enough to trigger failsafe.
my $ret = $node->safe_psql(
	'postgres',
	qq[
SELECT datname,
       age(datfrozenxid) > current_setting('vacuum_failsafe_age')::int as old
FROM pg_database ORDER BY 1
]);
is( $ret, "postgres|t
template0|t
template1|t", "all tables became old");

my $log_offset = -s $node->logfile;

# Finish the old transaction, to allow vacuum freezing to advance
# relfrozenxid and datfrozenxid again.
$background_psql->query_safe(qq[COMMIT]);
$background_psql->quit;

# Wait until autovacuum processed all tables and advanced the
# system-wide oldest-XID.
$node->poll_query_until(
	'postgres', qq[
SELECT NOT EXISTS (
	SELECT *
	FROM pg_database
	WHERE age(datfrozenxid) > current_setting('autovacuum_freeze_max_age')::int)
]) or die "timeout waiting for all databases to be vacuumed";

# Check if these tables are vacuumed.
$ret = $node->safe_psql(
	'postgres', qq[
SELECT relname, age(relfrozenxid) > current_setting('autovacuum_freeze_max_age')::int
FROM pg_class
WHERE relname IN ('large', 'large_trunc', 'small', 'small_trunc')
ORDER BY 1
]);

is( $ret, "large|f
large_trunc|f
small|f
small_trunc|f", "all tables are vacuumed");

# Check if vacuum failsafe was triggered for each table.
my $log_contents = slurp_file($node->logfile, $log_offset);
foreach my $tablename ('large', 'large_trunc', 'small', 'small_trunc')
{
	like(
		$log_contents,
		qr/bypassing nonessential maintenance of table "postgres.public.$tablename" as a failsafe after \d+ index scans/,
		"failsafe vacuum triggered for $tablename");
}

$node->stop;
done_testing();
