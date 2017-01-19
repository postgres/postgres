# Basic logical replication test
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 11;

# Initialize publisher node
my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Create some preexisting content on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_notrep AS SELECT generate_series(1,10) AS a");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_ins (a int)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_full AS SELECT generate_series(1,10) AS a");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rep (a int primary key)");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_notrep (a int)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_ins (a int)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_full (a int)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rep (a int primary key)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub");
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_ins_only WITH (nopublish delete, nopublish update)");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub ADD TABLE tab_rep, tab_full");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_ins_only ADD TABLE tab_ins");

my $appname = 'tap_sub';
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub, tap_pub_ins_only");

# Wait for subscriber to finish initialization
my $caughtup_query =
"SELECT pg_current_xlog_location() <= replay_location FROM pg_stat_replication WHERE application_name = '$appname';";
$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

my $result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_notrep");
is($result, qq(0), 'check non-replicated table is empty on subscriber');

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_ins SELECT generate_series(1,50)");
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab_ins WHERE a > 20");
$node_publisher->safe_psql('postgres',
	"UPDATE tab_ins SET a = -a");

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rep SELECT generate_series(1,50)");
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab_rep WHERE a > 20");
$node_publisher->safe_psql('postgres',
	"UPDATE tab_rep SET a = -a");

$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), min(a), max(a) FROM tab_ins");
is($result, qq(50|1|50), 'check replicated inserts on subscriber');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), min(a), max(a) FROM tab_rep");
is($result, qq(20|-20|-1), 'check replicated changes on subscriber');

# insert some duplicate rows
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_full SELECT generate_series(1,10)");

# add REPLICA IDENTITY FULL so we can update
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_full REPLICA IDENTITY FULL");
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_full REPLICA IDENTITY FULL");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_ins REPLICA IDENTITY FULL");
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_ins REPLICA IDENTITY FULL");

# and do the update
$node_publisher->safe_psql('postgres',
	"UPDATE tab_full SET a = a * a");

# Wait for subscription to catch up
$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), min(a), max(a) FROM tab_full");
is($result, qq(10|1|100), 'update works with REPLICA IDENTITY FULL and duplicate tuples');

# check that change of connection string and/or publication list causes
# restart of subscription workers. Not all of these are registered as tests
# as we need to poll for a change but the test suite will fail none the less
# when something goes wrong.
my $oldpid = $node_publisher->safe_psql('postgres',
	"SELECT pid FROM pg_stat_replication WHERE application_name = '$appname';");
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub CONNECTION 'application_name=$appname $publisher_connstr'");
$node_publisher->poll_query_until('postgres',
	"SELECT pid != $oldpid FROM pg_stat_replication WHERE application_name = '$appname';")
  or die "Timed out while waiting for apply to restart";

$oldpid = $node_publisher->safe_psql('postgres',
	"SELECT pid FROM pg_stat_replication WHERE application_name = '$appname';");
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub SET PUBLICATION tap_pub_ins_only");
$node_publisher->poll_query_until('postgres',
	"SELECT pid != $oldpid FROM pg_stat_replication WHERE application_name = '$appname';")
  or die "Timed out while waiting for apply to restart";

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_ins SELECT generate_series(1001,1100)");
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab_rep");

$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), min(a), max(a) FROM tab_ins");
is($result, qq(150|1|1100), 'check replicated inserts after subscription publication change');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), min(a), max(a) FROM tab_rep");
is($result, qq(20|-20|-1), 'check changes skipped after subscription publication change');

# check alter publication (relcache invalidation etc)
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_ins_only WITH (publish delete)");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_ins_only ADD TABLE tab_full");
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab_ins WHERE a > 0");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_full VALUES(0)");

$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

# note that data are different on provider and subscriber
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), min(a), max(a) FROM tab_ins");
is($result, qq(50|1|50), 'check replicated deletes after alter publication');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), min(a), max(a) FROM tab_full");
is($result, qq(11|0|100), 'check replicated insert after alter publication');

# check all the cleanup
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub");

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM pg_subscription");
is($result, qq(0), 'check subscription was dropped on subscriber');

$result =
  $node_publisher->safe_psql('postgres', "SELECT count(*) FROM pg_replication_slots");
is($result, qq(0), 'check replication slot was dropped on publisher');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM pg_replication_origin");
is($result, qq(0), 'check replication origin was dropped on subscriber');

$node_subscriber->stop('fast');
$node_publisher->stop('fast');
