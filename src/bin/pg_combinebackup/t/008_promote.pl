# Copyright (c) 2021-2025, PostgreSQL Global Development Group
#
# Test whether WAL summaries are complete such that incremental backup
# can be performed after promoting a standby at an arbitrary LSN.

use strict;
use warnings FATAL => 'all';
use File::Compare;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Can be changed to test the other modes.
my $mode = $ENV{PG_TEST_PG_COMBINEBACKUP_MODE} || '--copy';

note "testing using mode $mode";

# Set up a new database instance.
my $node1 = PostgreSQL::Test::Cluster->new('node1');
$node1->init(has_archiving => 1, allows_streaming => 1);
$node1->append_conf('postgresql.conf', 'summarize_wal = on');
$node1->append_conf('postgresql.conf', 'log_min_messages = debug1');
$node1->start;

# Create a table and insert a test row into it.
$node1->safe_psql('postgres', <<EOM);
CREATE TABLE mytable (a int, b text);
INSERT INTO mytable VALUES (1, 'avocado');
EOM

# Take a full backup.
my $backup1path = $node1->backup_dir . '/backup1';
$node1->command_ok(
	[
		'pg_basebackup',
		'--pgdata' => $backup1path,
		'--no-sync',
		'--checkpoint' => 'fast',
	],
	"full backup from node1");

# Checkpoint and record LSN after.
$node1->safe_psql('postgres', 'CHECKPOINT');
my $lsn = $node1->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');

# Insert a second row on the original node.
$node1->safe_psql('postgres', <<EOM);
INSERT INTO mytable VALUES (2, 'beetle');
EOM

# Now create a second node. We want this to stream from the first node and
# then stop recovery at some arbitrary LSN, not just when it hits the end of
# WAL, so use a recovery target.
my $node2 = PostgreSQL::Test::Cluster->new('node2');
$node2->init_from_backup($node1, 'backup1', has_streaming => 1);
$node2->append_conf('postgresql.conf', <<EOM);
recovery_target_lsn = '$lsn'
recovery_target_action = 'pause'
EOM
$node2->start();

# Wait until recovery pauses, then promote.
$node2->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused';");
$node2->safe_psql('postgres', "SELECT pg_promote()");

# Once promotion occurs, insert a second row on the new node.
$node2->poll_query_until('postgres', "SELECT pg_is_in_recovery() = 'f';");
$node2->safe_psql('postgres', <<EOM);
INSERT INTO mytable VALUES (2, 'blackberry');
EOM

# Now take an incremental backup. If WAL summarization didn't follow the
# timeline change correctly, something should break at this point.
my $backup2path = $node1->backup_dir . '/backup2';
$node2->command_ok(
	[
		'pg_basebackup',
		'--pgdata' => $backup2path,
		'--no-sync',
		'--checkpoint' => 'fast',
		'--incremental' => $backup1path . '/backup_manifest',
	],
	"incremental backup from node2");

# Restore the incremental backup and use it to create a new node.
my $node3 = PostgreSQL::Test::Cluster->new('node3');
$node3->init_from_backup($node1, 'backup2',
	combine_with_prior => ['backup1']);
$node3->start();

done_testing();
