# Checks for BEGIN WAIT FOR LSN
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 8;

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->start;

# And some content and take a backup
$node_master->safe_psql('postgres',
	"CREATE TABLE wait_test AS SELECT generate_series(1,10) AS a");
my $backup_name = 'my_backup';
$node_master->backup($backup_name);

# Using the backup, create a streaming standby with a 1 second delay
my $node_standby = get_new_node('standby');
my $delay        = 1;
$node_standby->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
$node_standby->append_conf('postgresql.conf', qq[
	recovery_min_apply_delay = '${delay}s'
]);
$node_standby->start;


# Check that timeouts make us wait for the specified time (1s here)
my $current_time = $node_standby->safe_psql('postgres', "SELECT now()");
my $two_seconds = 2000; # in milliseconds
my $start_time = time();
$node_standby->safe_psql('postgres',
	"BEGIN WAIT FOR LSN '0/FFFFFFFF' TIMEOUT $two_seconds");
my $time_waited = (time() - $start_time) * 1000; # convert to milliseconds
ok($time_waited >= $two_seconds, "WAIT FOR TIMEOUT waits for enough time");


# Check that timeouts let us stop waiting right away, before reaching target LSN
$node_master->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(11, 20))");
my $lsn1 = $node_master->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
my ($ret, $out, $err) = $node_standby->psql('postgres',
	"BEGIN WAIT FOR LSN '$lsn1' TIMEOUT 1");

ok($ret == 0, "zero return value when failed to WAIT FOR LSN on standby");
ok($err =~ /WARNING:  didn't start transaction because LSN was not reached/,
	"correct error message when failed to WAIT FOR LSN on standby");
ok($out eq "f", "if given too little wait time, WAIT doesn't reach target LSN");


# Check that WAIT FOR works fine and reaches target LSN if given no timeout

# Add data on master, memorize master's last LSN
$node_master->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(21, 30))");
my $lsn2 = $node_master->safe_psql('postgres', "SELECT pg_current_wal_lsn()");

# Wait for it to appear on replica, memorize replica's last LSN
$node_standby->safe_psql('postgres',
	"BEGIN WAIT FOR LSN '$lsn2'");
my $reached_lsn = $node_standby->safe_psql('postgres',
	"SELECT pg_last_wal_replay_lsn()");

# Make sure that master's and replica's LSNs are the same after WAIT
my $compare_lsns = $node_standby->safe_psql('postgres',
	"SELECT pg_lsn_cmp('$reached_lsn'::pg_lsn, '$lsn2'::pg_lsn)");
ok($compare_lsns eq 0,
	"standby reached the same LSN as master before starting transaction");


# Make sure that it's not allowed to use WAIT FOR on master
($ret, $out, $err) = $node_master->psql('postgres',
	"BEGIN WAIT FOR LSN '0/FFFFFFFF'");

ok($ret != 0, "non-zero return value when trying to WAIT FOR LSN on master");
ok($err =~ /ERROR:  WAIT FOR can only be used on standby/,
	"correct error message when trying to WAIT FOR LSN on master");
ok($out eq '', "empty output when trying to WAIT FOR LSN on master");


$node_standby->stop;
$node_master->stop;
