# Test for replication slot limit
# Ensure that max_slot_wal_keep_size limits the number of WAL files to
# be kept by replication slots.
use strict;
use warnings;

use TestLib;
use PostgresNode;

use File::Path qw(rmtree);
use Test::More tests => 14;
use Time::HiRes qw(usleep);

$ENV{PGDATABASE} = 'postgres';

# Initialize master node, setting wal-segsize to 1MB
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1, extra => ['--wal-segsize=1']);
$node_master->append_conf(
	'postgresql.conf', qq(
min_wal_size = 2MB
max_wal_size = 4MB
log_checkpoints = yes
));
$node_master->start;
$node_master->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('rep1')");

# The slot state and remain should be null before the first connection
my $result = $node_master->safe_psql('postgres',
	"SELECT restart_lsn IS NULL, wal_status is NULL, safe_wal_size is NULL FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "t|t|t", 'check the state of non-reserved slot is "unknown"');


# Take backup
my $backup_name = 'my_backup';
$node_master->backup($backup_name);

# Create a standby linking to it using the replication slot
my $node_standby = get_new_node('standby_1');
$node_standby->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
$node_standby->append_conf('postgresql.conf', "primary_slot_name = 'rep1'");

$node_standby->start;

# Wait until standby has replayed enough data
my $start_lsn = $node_master->lsn('write');
$node_master->wait_for_catchup($node_standby, 'replay', $start_lsn);

# Stop standby
$node_standby->stop;

# Preparation done, the slot is the state "reserved" now
$result = $node_master->safe_psql('postgres',
	"SELECT wal_status, safe_wal_size IS NULL FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "reserved|t", 'check the catching-up state');

# Advance WAL by five segments (= 5MB) on master
advance_wal($node_master, 1);
$node_master->safe_psql('postgres', "CHECKPOINT;");

# The slot is always "safe" when fitting max_wal_size
$result = $node_master->safe_psql('postgres',
	"SELECT wal_status, safe_wal_size IS NULL FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "reserved|t",
	'check that it is safe if WAL fits in max_wal_size');

advance_wal($node_master, 4);
$node_master->safe_psql('postgres', "CHECKPOINT;");

# The slot is always "safe" when max_slot_wal_keep_size is not set
$result = $node_master->safe_psql('postgres',
	"SELECT wal_status, safe_wal_size IS NULL FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "reserved|t", 'check that slot is working');

# The standby can reconnect to master
$node_standby->start;

$start_lsn = $node_master->lsn('write');
$node_master->wait_for_catchup($node_standby, 'replay', $start_lsn);

$node_standby->stop;

# Set max_slot_wal_keep_size on master
my $max_slot_wal_keep_size_mb = 6;
$node_master->append_conf(
	'postgresql.conf', qq(
max_slot_wal_keep_size = ${max_slot_wal_keep_size_mb}MB
));
$node_master->reload;

# The slot is in safe state.

$result = $node_master->safe_psql('postgres',
	"SELECT wal_status FROM pg_replication_slots WHERE slot_name = 'rep1'");
is($result, "reserved", 'check that max_slot_wal_keep_size is working');

# Advance WAL again then checkpoint, reducing remain by 2 MB.
advance_wal($node_master, 2);
$node_master->safe_psql('postgres', "CHECKPOINT;");

# The slot is still working
$result = $node_master->safe_psql('postgres',
	"SELECT wal_status FROM pg_replication_slots WHERE slot_name = 'rep1'");
is($result, "reserved",
	'check that safe_wal_size gets close to the current LSN');

# The standby can reconnect to master
$node_standby->start;
$start_lsn = $node_master->lsn('write');
$node_master->wait_for_catchup($node_standby, 'replay', $start_lsn);
$node_standby->stop;

# wal_keep_size overrides max_slot_wal_keep_size
$result = $node_master->safe_psql('postgres',
	"ALTER SYSTEM SET wal_keep_size to '8MB'; SELECT pg_reload_conf();");
# Advance WAL again then checkpoint, reducing remain by 6 MB.
advance_wal($node_master, 6);
$result = $node_master->safe_psql('postgres',
	"SELECT wal_status as remain FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "extended",
	'check that wal_keep_size overrides max_slot_wal_keep_size');
# restore wal_keep_size
$result = $node_master->safe_psql('postgres',
	"ALTER SYSTEM SET wal_keep_size to 0; SELECT pg_reload_conf();");

# The standby can reconnect to master
$node_standby->start;
$start_lsn = $node_master->lsn('write');
$node_master->wait_for_catchup($node_standby, 'replay', $start_lsn);
$node_standby->stop;

# Advance WAL again without checkpoint, reducing remain by 6 MB.
advance_wal($node_master, 6);

# Slot gets into 'reserved' state
$result = $node_master->safe_psql('postgres',
	"SELECT wal_status FROM pg_replication_slots WHERE slot_name = 'rep1'");
is($result, "extended", 'check that the slot state changes to "extended"');

# do checkpoint so that the next checkpoint runs too early
$node_master->safe_psql('postgres', "CHECKPOINT;");

# Advance WAL again without checkpoint; remain goes to 0.
advance_wal($node_master, 1);

# Slot gets into 'unreserved' state and safe_wal_size is negative
$result = $node_master->safe_psql('postgres',
	"SELECT wal_status, safe_wal_size <= 0 FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "unreserved|t",
	'check that the slot state changes to "unreserved"');

# The standby still can connect to master before a checkpoint
$node_standby->start;

$start_lsn = $node_master->lsn('write');
$node_master->wait_for_catchup($node_standby, 'replay', $start_lsn);

$node_standby->stop;

ok( !find_in_log(
		$node_standby,
		"requested WAL segment [0-9A-F]+ has already been removed"),
	'check that required WAL segments are still available');

# Advance WAL again, the slot loses the oldest segment.
my $logstart = get_log_size($node_master);
advance_wal($node_master, 7);
$node_master->safe_psql('postgres', "CHECKPOINT;");

# WARNING should be issued
ok( find_in_log(
		$node_master,
		"invalidating slot \"rep1\" because its restart_lsn [0-9A-F/]+ exceeds max_slot_wal_keep_size",
		$logstart),
	'check that the warning is logged');

# This slot should be broken
$result = $node_master->safe_psql('postgres',
	"SELECT slot_name, active, restart_lsn IS NULL, wal_status, safe_wal_size FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "rep1|f|t|lost|",
	'check that the slot became inactive and the state "lost" persists');

# The standby no longer can connect to the master
$logstart = get_log_size($node_standby);
$node_standby->start;

my $failed = 0;
for (my $i = 0; $i < 10000; $i++)
{
	if (find_in_log(
			$node_standby,
			"requested WAL segment [0-9A-F]+ has already been removed",
			$logstart))
	{
		$failed = 1;
		last;
	}
	usleep(100_000);
}
ok($failed, 'check that replication has been broken');

$node_master->stop('immediate');
$node_standby->stop('immediate');

my $node_master2 = get_new_node('master2');
$node_master2->init(allows_streaming => 1);
$node_master2->append_conf(
	'postgresql.conf', qq(
min_wal_size = 32MB
max_wal_size = 32MB
log_checkpoints = yes
));
$node_master2->start;
$node_master2->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('rep1')");
$backup_name = 'my_backup2';
$node_master2->backup($backup_name);

$node_master2->stop;
$node_master2->append_conf(
	'postgresql.conf', qq(
max_slot_wal_keep_size = 0
));
$node_master2->start;

$node_standby = get_new_node('standby_2');
$node_standby->init_from_backup($node_master2, $backup_name,
	has_streaming => 1);
$node_standby->append_conf('postgresql.conf', "primary_slot_name = 'rep1'");
$node_standby->start;
my @result =
  split(
	'\n',
	$node_master2->safe_psql(
		'postgres',
		"CREATE TABLE tt();
		 DROP TABLE tt;
		 SELECT pg_switch_wal();
		 CHECKPOINT;
		 SELECT 'finished';",
		timeout => '60'));
is($result[1], 'finished', 'check if checkpoint command is not blocked');

#####################################
# Advance WAL of $node by $n segments
sub advance_wal
{
	my ($node, $n) = @_;

	# Advance by $n segments (= (16 * $n) MB) on master
	for (my $i = 0; $i < $n; $i++)
	{
		$node->safe_psql('postgres',
			"CREATE TABLE t (); DROP TABLE t; SELECT pg_switch_wal();");
	}
	return;
}

# return the size of logfile of $node in bytes
sub get_log_size
{
	my ($node) = @_;

	return (stat $node->logfile)[7];
}

# find $pat in logfile of $node after $off-th byte
sub find_in_log
{
	my ($node, $pat, $off) = @_;

	$off = 0 unless defined $off;
	my $log = TestLib::slurp_file($node->logfile);
	return 0 if (length($log) <= $off);

	$log = substr($log, $off);

	return $log =~ m/$pat/;
}
