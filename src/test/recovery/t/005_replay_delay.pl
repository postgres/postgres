# Checks for recovery_min_apply_delay
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 1;

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->start;

# And some content
$node_master->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1, 10) AS a");

# Take backup
my $backup_name = 'my_backup';
$node_master->backup($backup_name);

# Create streaming standby from backup
my $node_standby = get_new_node('standby');
my $delay        = 3;
$node_standby->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
$node_standby->append_conf(
	'recovery.conf', qq(
recovery_min_apply_delay = '${delay}s'
));
$node_standby->start;

# Make new content on master and check its presence in standby depending
# on the delay applied above. Before doing the insertion, get the
# current timestamp that will be used as a comparison base. Even on slow
# machines, this allows to have a predictable behavior when comparing the
# delay between data insertion moment on master and replay time on standby.
my $master_insert_time = time();
$node_master->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(11, 20))");

# Now wait for replay to complete on standby. We're done waiting when the
# standby has replayed up to the previously saved master LSN.
my $until_lsn =
  $node_master->safe_psql('postgres', "SELECT pg_current_wal_lsn()");

$node_standby->poll_query_until('postgres',
	"SELECT (pg_last_wal_replay_lsn() - '$until_lsn'::pg_lsn) >= 0")
  or die "standby never caught up";

# This test is successful if and only if the LSN has been applied with at least
# the configured apply delay.
ok(time() - $master_insert_time >= $delay,
	"standby applies WAL only after replication delay");
