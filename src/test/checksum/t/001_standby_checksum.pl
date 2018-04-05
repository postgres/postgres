# Test suite for testing enabling data checksums with streaming replication
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 10;

my $MAX_TRIES = 30;

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->start;
my $backup_name = 'my_backup';

# Take backup
$node_master->backup($backup_name);

# Create streaming standby linking to master
my $node_standby_1 = get_new_node('standby_1');
$node_standby_1->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
$node_standby_1->start;

# Create some content on master to have un-checksummed data in the cluster
$node_master->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");

# Wait for standbys to catch up
$node_master->wait_for_catchup($node_standby_1, 'replay',
	$node_master->lsn('insert'));

# Check that checksums are turned off
my $result = $node_master->safe_psql('postgres',
	"SELECT setting FROM pg_catalog.pg_settings WHERE name = 'data_checksums';");
is($result, "off", 'ensure checksums are turned off on master');

$result = $node_standby_1->safe_psql('postgres',
	"SELECT setting FROM pg_catalog.pg_settings WHERE name = 'data_checksums';");
is($result, "off", 'ensure checksums are turned off on standby_1');

# Enable checksums for the cluster
$node_master->safe_psql('postgres', "SELECT pg_enable_data_checksums();");

# Ensure that the master has switched to inprogress immediately
$result = $node_master->safe_psql('postgres',
	"SELECT setting FROM pg_catalog.pg_settings WHERE name = 'data_checksums';");
is($result, "inprogress", 'ensure checksums are in progress on master');

# Wait for checksum enable to be replayed
$node_master->wait_for_catchup($node_standby_1, 'replay');

# Ensure that the standby has switched to inprogress
$result = $node_standby_1->safe_psql('postgres',
	"SELECT setting FROM pg_catalog.pg_settings WHERE name = 'data_checksums';");
is($result, "inprogress", 'ensure checksums are in progress on standby_1');

# Insert some more data which should be checksummed on INSERT
$node_master->safe_psql('postgres',
	"INSERT INTO t VALUES (generate_series(1,10000));");

# Wait for checksums enabled on the master
for (my $i = 0; $i < $MAX_TRIES; $i++)
{
	$result = $node_master->safe_psql('postgres',
		"SELECT setting FROM pg_catalog.pg_settings WHERE name = 'data_checksums';");
	last if ($result eq 'on');
	sleep(1);
}
is ($result, "on", 'ensure checksums are enabled on master');

# Wait for checksums enabled on the standby
for (my $i = 0; $i < $MAX_TRIES; $i++)
{
	$result = $node_standby_1->safe_psql('postgres',
		"SELECT setting FROM pg_catalog.pg_settings WHERE name = 'data_checksums';");
	last if ($result eq 'on');
	sleep(1);
}
is ($result, "on", 'ensure checksums are enabled on standby');

$result = $node_master->safe_psql('postgres', "SELECT count(a) FROM t");
is ($result, "20000", 'ensure we can safely read all data with checksums');

# Disable checksums and ensure it's propagated to standby and that we can
# still read all data
$node_master->safe_psql('postgres', "SELECT pg_disable_data_checksums();");
$result = $node_master->safe_psql('postgres',
	"SELECT setting FROM pg_catalog.pg_settings WHERE name = 'data_checksums';");
is($result, "off", 'ensure checksums are in progress on master');

# Wait for checksum disable to be replayed
$node_master->wait_for_catchup($node_standby_1, 'replay');

# Ensure that the standby has switched to off
$result = $node_standby_1->safe_psql('postgres',
	"SELECT setting FROM pg_catalog.pg_settings WHERE name = 'data_checksums';");
is($result, "off", 'ensure checksums are in progress on standby_1');

$result = $node_master->safe_psql('postgres', "SELECT count(a) FROM t");
is ($result, "20000", 'ensure we can safely read all data without checksums');
