# Copyright (c) 2026, PostgreSQL Global Development Group

# Test autovacuum's handling of TOAST storage parameters

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Create a test node with autovacuum disabled.
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
autovacuum = off
autovacuum_naptime = '1s'
});
$node->start;

# Create TOAST table that is eligible for autovacuum due to inherited relopts.
$node->safe_psql(
	'postgres', qq{
  CREATE TABLE toast_relopts (i int, j text STORAGE EXTERNAL) WITH
    (autovacuum_enabled = false, toast.autovacuum_enabled = true,
     autovacuum_vacuum_threshold = 1,
     autovacuum_vacuum_scale_factor = 0,
     autovacuum_vacuum_insert_threshold = 1,
     autovacuum_vacuum_insert_scale_factor = 0,
     vacuum_truncate = false);
  INSERT INTO toast_relopts VALUES (1, repeat('a', 10000)), (2, repeat('b', 10000));
  SELECT pg_stat_force_next_flush();
});

# Get TOAST table's OID for following commands.
my $toast = $node->safe_psql('postgres',
	"SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 'toast_relopts'::regclass"
);

# Verify scores view used inherited insert threshold.
is( $node->safe_psql(
		'postgres', qq{
  SELECT vacuum_insert_score > 1 FROM pg_stat_autovacuum_scores
  WHERE relid = '$toast'::regclass
}),
	't',
	'inherited insert threshold in pg_stat_autovacuum_scores');

# Delete all rows so that we can verify inherited vacuum_truncate takes effect.
$node->safe_psql('postgres', 'DELETE FROM toast_relopts');

# Enable autovacuum.
$node->append_conf('postgresql.conf', 'autovacuum = on');
$node->reload;

# Wait until autovacuum processes the table.
ok( $node->poll_query_until(
		'postgres', qq{
  SELECT last_autovacuum IS NOT NULL FROM pg_stat_all_tables
  WHERE relid = '$toast'::regclass
}),
	'autovacuum of a TOAST table with inherited thresholds');

# Verify autovacuum didn't truncate the table.
is($node->safe_psql('postgres', "SELECT pg_relation_size('$toast') > 0"),
	't', 'inherited vacuum_truncate');

$node->stop;
done_testing();
