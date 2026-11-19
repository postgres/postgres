
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Check that a concurrent transaction doesn't cause false negatives in
# pg_check_visible() function
#
# Also check that a standby can restart after replaying a VM clear followed by
# a VM truncation.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;


# Initialize the primary node
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = 1h
});
$node->start;

# Initialize the streaming standby
my $backup_name = 'my_backup';
$node->backup($backup_name);
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, $backup_name, has_streaming => 1);
$standby->start;

# Setup another database
$node->safe_psql("postgres", "CREATE DATABASE other_database;\n");
my $bsession = $node->background_psql('other_database');

# Run a concurrent transaction
$bsession->query_safe(
	qq[
	BEGIN;
	SELECT txid_current();
]);

# Create a sample table and run vacuum
$node->safe_psql("postgres",
		"CREATE EXTENSION pg_visibility;\n"
	  . "CREATE TABLE vacuum_test AS SELECT 42 i;\n"
	  . "VACUUM (disable_page_skipping) vacuum_test;");

# Run pg_check_visible()
my $result = $node->safe_psql("postgres",
	"SELECT * FROM pg_check_visible('vacuum_test');");

# There should be no false negatives
ok($result eq "", "pg_check_visible() detects no errors");

# Run pg_check_visible() on standby
$node->wait_for_catchup($standby);
$result = $standby->safe_psql("postgres",
	"SELECT * FROM pg_check_visible('vacuum_test');");

# There should be no false negatives either
ok($result eq "", "pg_check_visible() detects no errors");

# End the concurrent transaction
$bsession->query_safe("COMMIT;");
$bsession->quit;

# A WAL record clearing a VM bit registers the VM block. Make sure a standby
# that crashes after the VM fork was truncated, and so replays the clear again
# against a missing VM block, doesn't leave an unresolved invalid-page reference
# behind: the VM truncation record must forget it.
$node->safe_psql(
	"postgres",
	"CREATE TABLE vm_truncate_test AS SELECT generate_series(1, 500) i;\n"
	  . "VACUUM (FREEZE) vm_truncate_test;");

# Move the standby's restart point past the VM set but before the VM clear.
$node->safe_psql("postgres", "CHECKPOINT;");
$node->wait_for_catchup($standby);
$standby->safe_psql("postgres", "CHECKPOINT;");

# Without full page images, the VM clear is replayed as a plain delta.
$node->append_conf('postgresql.conf', "full_page_writes = off");
$node->reload;
$node->poll_query_until(
	"postgres",
	q{SELECT current_setting('full_page_writes') = 'off';});

$node->safe_psql(
	"postgres",
	"UPDATE vm_truncate_test SET i = 0 WHERE i = 1;\n"
	  . "SELECT pg_truncate_visibility_map('vm_truncate_test');");
$node->wait_for_catchup($standby);

is($standby->safe_psql(
		"postgres", "SELECT pg_relation_size('vm_truncate_test', 'vm');"),
	'0',
	"VM fork truncated on standby");

# Crash the standby; redo restarts before the VM clear.
$standby->stop('immediate');
my $log_offset = -s $standby->logfile;
ok($standby->start(fail_ok => 1), "standby restarts after VM truncation");
ok(!$standby->log_contains(
		qr/WAL contains references to invalid pages/, $log_offset),
	"no invalid-page PANIC during recovery");

$node->stop;
$standby->stop;

done_testing();
