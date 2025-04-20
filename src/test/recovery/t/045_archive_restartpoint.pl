
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

# Test restartpoints during archive recovery.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $archive_max_mb = 320;
my $wal_segsize = 1;

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(
	has_archiving => 1,
	allows_streaming => 1,
	extra => [ '--wal-segsize' => $wal_segsize ]);
$node_primary->start;
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

$node_primary->safe_psql('postgres',
	('DO $$BEGIN FOR i IN 1..' . $archive_max_mb / $wal_segsize)
	  . ' LOOP CHECKPOINT; PERFORM pg_switch_wal(); END LOOP; END$$;');

# Force archiving of WAL file containing recovery target
my $until_lsn = $node_primary->lsn('write');
$node_primary->safe_psql('postgres', "SELECT pg_switch_wal()");
$node_primary->stop;

# Archive recovery
my $node_restore = PostgreSQL::Test::Cluster->new('restore');
$node_restore->init_from_backup($node_primary, $backup_name,
	has_restoring => 1);
$node_restore->append_conf('postgresql.conf',
	"recovery_target_lsn = '$until_lsn'");
$node_restore->append_conf('postgresql.conf',
	'recovery_target_action = pause');
$node_restore->append_conf('postgresql.conf',
	'max_wal_size = ' . 2 * $wal_segsize);
$node_restore->append_conf('postgresql.conf', 'log_checkpoints = on');

$node_restore->start;

# Wait until restore has replayed enough data
my $caughtup_query =
  "SELECT '$until_lsn'::pg_lsn <= pg_last_wal_replay_lsn()";
$node_restore->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for restore to catch up";

$node_restore->stop;
ok(1, 'restore caught up');

done_testing();
