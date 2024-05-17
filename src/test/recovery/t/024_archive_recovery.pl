
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test for archive recovery of WAL generated with wal_level=minimal
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Initialize and start node with wal_level = replica and WAL archiving
# enabled.
my $node = PostgreSQL::Test::Cluster->new('orig');
$node->init(has_archiving => 1, allows_streaming => 1);
my $replica_config = q[
wal_level = replica
archive_mode = on
max_wal_senders = 10
hot_standby = off
];
$node->append_conf('postgresql.conf', $replica_config);
$node->start;

# Take backup
my $backup_name = 'my_backup';
$node->backup($backup_name);

# Restart node with wal_level = minimal and WAL archiving disabled
# to generate WAL with that setting. Note that such WAL has not been
# archived yet at this moment because WAL archiving is not enabled.
$node->append_conf(
	'postgresql.conf', q[
wal_level = minimal
archive_mode = off
max_wal_senders = 0
]);
$node->restart;

# Restart node with wal_level = replica and WAL archiving enabled
# to archive WAL previously generated with wal_level = minimal.
# We ensure the WAL file containing the record indicating the change
# of wal_level to minimal is archived by checking pg_stat_archiver.
$node->append_conf('postgresql.conf', $replica_config);
$node->restart;

# Find next WAL segment to be archived
my $walfile_to_be_archived = $node->safe_psql('postgres',
	"SELECT pg_walfile_name(pg_current_wal_lsn());");

# Make WAL segment eligible for archival
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
my $archive_wait_query =
  "SELECT '$walfile_to_be_archived' <= last_archived_wal FROM pg_stat_archiver;";

# Wait until the WAL segment has been archived.
$node->poll_query_until('postgres', $archive_wait_query)
  or die "Timed out while waiting for WAL segment to be archived";

$node->stop;

# Initialize new node from backup, and start archive recovery. Check that
# archive recovery fails with an error when it detects the WAL record
# indicating the change of wal_level to minimal and node stops.
sub test_recovery_wal_level_minimal
{
	my ($node_name, $node_text, $standby_setting) = @_;

	my $recovery_node = PostgreSQL::Test::Cluster->new($node_name);
	$recovery_node->init_from_backup(
		$node, $backup_name,
		has_restoring => 1,
		standby => $standby_setting);

	# Use run_log instead of recovery_node->start because this test expects
	# that the server ends with an error during recovery.
	run_log(
		[
			'pg_ctl', '-D',
			$recovery_node->data_dir, '-l',
			$recovery_node->logfile, 'start'
		]);

	# wait for postgres to terminate
	foreach my $i (0 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
	{
		last if !-f $recovery_node->data_dir . '/postmaster.pid';
		usleep(100_000);
	}

	# Confirm that the archive recovery fails with an expected error
	my $logfile = slurp_file($recovery_node->logfile());
	ok( $logfile =~
		  qr/FATAL: .* WAL was generated with "wal_level=minimal", cannot continue recovering/,
		"$node_text ends with an error because it finds WAL generated with \"wal_level=minimal\""
	);
}

# Test for archive recovery
test_recovery_wal_level_minimal('archive_recovery', 'archive recovery', 0);

# Test for standby server
test_recovery_wal_level_minimal('standby', 'standby', 1);

done_testing();
