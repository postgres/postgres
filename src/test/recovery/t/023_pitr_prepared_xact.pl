
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test for point-in-time recovery (PITR) with prepared transactions
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Compare;

# Initialize and start primary node with WAL archiving
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(has_archiving => 1, allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', qq{
max_prepared_transactions = 10});
$node_primary->start;

# Take backup
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Initialize node for PITR targeting a very specific restore point, just
# after a PREPARE TRANSACTION is issued so as we finish with a promoted
# node where this 2PC transaction needs an explicit COMMIT PREPARED.
my $node_pitr = PostgreSQL::Test::Cluster->new('node_pitr');
$node_pitr->init_from_backup(
	$node_primary, $backup_name,
	standby => 0,
	has_restoring => 1);
$node_pitr->append_conf(
	'postgresql.conf', qq{
recovery_target_name = 'rp'
recovery_target_action = 'promote'});

# Workload with a prepared transaction and the target restore point.
$node_primary->psql(
	'postgres', qq{
CREATE TABLE foo(i int);
BEGIN;
INSERT INTO foo VALUES(1);
PREPARE TRANSACTION 'fooinsert';
SELECT pg_create_restore_point('rp');
INSERT INTO foo VALUES(2);
});

# Find next WAL segment to be archived
my $walfile_to_be_archived = $node_primary->safe_psql('postgres',
	"SELECT pg_walfile_name(pg_current_wal_lsn());");

# Make WAL segment eligible for archival
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal()');

# Wait until the WAL segment has been archived.
my $archive_wait_query =
  "SELECT '$walfile_to_be_archived' <= last_archived_wal FROM pg_stat_archiver;";
$node_primary->poll_query_until('postgres', $archive_wait_query)
  or die "Timed out while waiting for WAL segment to be archived";
my $last_archived_wal_file = $walfile_to_be_archived;

# Now start the PITR node.
$node_pitr->start;

# Wait until the PITR node exits recovery.
$node_pitr->poll_query_until('postgres', "SELECT pg_is_in_recovery() = 'f';")
  or die "Timed out while waiting for PITR promotion";

# Commit the prepared transaction in the latest timeline and check its
# result.  There should only be one row in the table, coming from the
# prepared transaction.  The row from the INSERT after the restore point
# should not show up, since our recovery target was older than the second
# INSERT done.
$node_pitr->psql('postgres', qq{COMMIT PREPARED 'fooinsert';});
my $result = $node_pitr->safe_psql('postgres', "SELECT * FROM foo;");
is($result, qq{1}, "check table contents after COMMIT PREPARED");

# Insert more data and do a checkpoint.  These should be generated on the
# timeline chosen after the PITR promotion.
$node_pitr->psql(
	'postgres', qq{
INSERT INTO foo VALUES(3);
CHECKPOINT;
});

# Enforce recovery, the checkpoint record generated previously should
# still be found.
$node_pitr->stop('immediate');
$node_pitr->start;

done_testing();
