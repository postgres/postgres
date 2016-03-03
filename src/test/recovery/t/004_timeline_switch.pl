# Test for timeline switch
# Ensure that a cascading standby is able to follow a newly-promoted standby
# on a new timeline.
use strict;
use warnings;
use File::Path qw(rmtree);
use PostgresNode;
use TestLib;
use Test::More tests => 1;

$ENV{PGDATABASE} = 'postgres';

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->start;

# Take backup
my $backup_name = 'my_backup';
$node_master->backup($backup_name);

# Create two standbys linking to it
my $node_standby_1 = get_new_node('standby_1');
$node_standby_1->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
$node_standby_1->start;
my $node_standby_2 = get_new_node('standby_2');
$node_standby_2->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
$node_standby_2->start;

# Create some content on master
$node_master->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1,1000) AS a");
my $until_lsn =
  $node_master->safe_psql('postgres', "SELECT pg_current_xlog_location();");

# Wait until standby has replayed enough data on standby 1
my $caughtup_query =
  "SELECT '$until_lsn'::pg_lsn <= pg_last_xlog_replay_location()";
$node_standby_1->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby to catch up";

# Stop and remove master, and promote standby 1, switching it to a new timeline
$node_master->teardown_node;
$node_standby_1->promote;

# Switch standby 2 to replay from standby 1
rmtree($node_standby_2->data_dir . '/recovery.conf');
my $connstr_1 = $node_standby_1->connstr;
$node_standby_2->append_conf(
	'recovery.conf', qq(
primary_conninfo='$connstr_1'
standby_mode=on
recovery_target_timeline='latest'
));
$node_standby_2->restart;

# Insert some data in standby 1 and check its presence in standby 2
# to ensure that the timeline switch has been done. Standby 1 needs
# to exit recovery first before moving on with the test.
$node_standby_1->poll_query_until('postgres',
	"SELECT pg_is_in_recovery() <> true");
$node_standby_1->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(1001,2000))");
$until_lsn =
  $node_standby_1->safe_psql('postgres', "SELECT pg_current_xlog_location();");
$caughtup_query =
  "SELECT '$until_lsn'::pg_lsn <= pg_last_xlog_replay_location()";
$node_standby_2->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby to catch up";

my $result =
  $node_standby_2->safe_psql('postgres', "SELECT count(*) FROM tab_int");
is($result, qq(2000), 'check content of standby 2');
