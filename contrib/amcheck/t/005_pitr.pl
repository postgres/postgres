# Copyright (c) 2021-2023, PostgreSQL Global Development Group

# Test integrity of intermediate states by PITR to those states
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# origin node: generate WAL records of interest.
my $origin = PostgreSQL::Test::Cluster->new('origin');
$origin->init(has_archiving => 1, allows_streaming => 1);
$origin->append_conf('postgresql.conf', 'autovacuum = off');
$origin->start;
$origin->backup('my_backup');
# Create a table with each of 6 PK values spanning 1/4 of a block.  Delete the
# first four, so one index leaf is eligible for deletion.  Make a replication
# slot just so pg_waldump will always have access to later WAL.
my $setup = <<EOSQL;
BEGIN;
CREATE EXTENSION amcheck;
CREATE TABLE not_leftmost (c text);
ALTER TABLE not_leftmost ALTER c SET STORAGE PLAIN;
INSERT INTO not_leftmost
  SELECT repeat(n::text, database_block_size / 4)
  FROM generate_series(1,6) t(n), pg_control_init();
ALTER TABLE not_leftmost ADD CONSTRAINT not_leftmost_pk PRIMARY KEY (c);
DELETE FROM not_leftmost WHERE c ~ '^[1-4]';
SELECT pg_create_physical_replication_slot('for_waldump', true, false);
COMMIT;
EOSQL
$origin->safe_psql('postgres', $setup);
my $before_vacuum_walfile =
  $origin->safe_psql('postgres', "SELECT pg_walfile_name(pg_current_wal_lsn())");
# VACUUM to delete the aforementioned leaf page.  Force an XLogFlush() by
# dropping a permanent table.  That way, the XLogReader infrastructure can
# always see VACUUM's records, even under synchronous_commit=off.  Finally,
# find the LSN of that VACUUM's last UNLINK_PAGE record.
my $vacuum = <<EOSQL;
SET synchronous_commit = off;
VACUUM (VERBOSE, INDEX_CLEANUP ON) not_leftmost;
CREATE TABLE XLogFlush ();
DROP TABLE XLogFlush;
SELECT pg_walfile_name(pg_current_wal_flush_lsn());
EOSQL
my $after_unlink_walfile = $origin->safe_psql('postgres', $vacuum);
$origin->stop;
my $unlink_lsn = do {
	my $stdout;
	run_log(['pg_waldump', '-p', $origin->data_dir . '/pg_wal',
			 $before_vacuum_walfile, $after_unlink_walfile],
			'>', \$stdout);
	$stdout =~ m|^rmgr: Btree .*, lsn: ([/0-9A-F]+), .*, desc: UNLINK_PAGE left|m;
	$1;
};
die "did not find UNLINK_PAGE record" unless $unlink_lsn;

# replica node: amcheck at notable points in the WAL stream
my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->init_from_backup($origin, 'my_backup', has_restoring => 1);
$replica->append_conf('postgresql.conf',
	"recovery_target_lsn = '$unlink_lsn'");
$replica->append_conf('postgresql.conf', 'recovery_target_inclusive = off');
$replica->append_conf('postgresql.conf', 'recovery_target_action = promote');
$replica->start;
$replica->poll_query_until('postgres', "SELECT pg_is_in_recovery() = 'f';")
  or die "Timed out while waiting for PITR promotion";
# recovery done; run amcheck
my $debug = "SET client_min_messages = 'debug1'";
my ($rc, $stderr);
$rc = $replica->psql(
	'postgres',
	"$debug; SELECT bt_index_parent_check('not_leftmost_pk', true)",
	stderr => \$stderr);
print STDERR $stderr, "\n";
is($rc, 0, "bt_index_parent_check passes");
like(
	$stderr,
	qr/interrupted page deletion detected/,
	"bt_index_parent_check: interrupted page deletion detected");
$rc = $replica->psql(
	'postgres',
	"$debug; SELECT bt_index_check('not_leftmost_pk', true)",
	stderr => \$stderr);
print STDERR $stderr, "\n";
is($rc, 0, "bt_index_check passes");

done_testing();
