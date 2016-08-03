# Minimal test testing streaming replication
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 4;

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

# Take backup of standby 1 (not mandatory, but useful to check if
# pg_basebackup works on a standby).
$node_standby_1->backup($backup_name);

# Create second standby node linking to standby 1
my $node_standby_2 = get_new_node('standby_2');
$node_standby_2->init_from_backup($node_standby_1, $backup_name,
	has_streaming => 1);
$node_standby_2->start;

# Create some content on master and check its presence in standby 1
$node_master->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1,1002) AS a");

# Wait for standbys to catch up
my $applname_1 = $node_standby_1->name;
my $applname_2 = $node_standby_2->name;
my $caughtup_query =
"SELECT pg_current_xlog_location() <= replay_location FROM pg_stat_replication WHERE application_name = '$applname_1';";
$node_master->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby 1 to catch up";
$caughtup_query =
"SELECT pg_last_xlog_replay_location() <= replay_location FROM pg_stat_replication WHERE application_name = '$applname_2';";
$node_standby_1->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby 2 to catch up";

my $result =
  $node_standby_1->safe_psql('postgres', "SELECT count(*) FROM tab_int");
print "standby 1: $result\n";
is($result, qq(1002), 'check streamed content on standby 1');

$result =
  $node_standby_2->safe_psql('postgres', "SELECT count(*) FROM tab_int");
print "standby 2: $result\n";
is($result, qq(1002), 'check streamed content on standby 2');

# Check that only READ-only queries can run on standbys
is($node_standby_1->psql('postgres', 'INSERT INTO tab_int VALUES (1)'),
	3, 'read-only queries on standby 1');
is($node_standby_2->psql('postgres', 'INSERT INTO tab_int VALUES (1)'),
	3, 'read-only queries on standby 2');
