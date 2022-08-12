# Basic logical replication test
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 27;

# Initialize publisher node
my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Create some preexisting content on publisher
$node_publisher->safe_psql(
	'postgres',
	"CREATE FUNCTION public.pg_get_replica_identity_index(int)
	 RETURNS regclass LANGUAGE sql AS 'SELECT 1/0'");    # shall not call
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_notrep AS SELECT generate_series(1,10) AS a");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_ins AS SELECT generate_series(1,1002) AS a");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_full AS SELECT generate_series(1,10) AS a");
$node_publisher->safe_psql('postgres', "CREATE TABLE tab_full2 (x text)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_full2 VALUES ('a'), ('b'), ('b')");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rep (a int primary key)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_mixed (a int primary key, b text, c numeric)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_mixed (a, b, c) VALUES (1, 'foo', 1.1)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_include (a int, b text, CONSTRAINT covering PRIMARY KEY(a) INCLUDE(b))"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_full_pk (a int primary key, b text)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_full_pk REPLICA IDENTITY FULL");
# Let this table with REPLICA IDENTITY NOTHING, allowing only INSERT changes.
$node_publisher->safe_psql('postgres', "CREATE TABLE tab_nothing (a int)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_nothing REPLICA IDENTITY NOTHING");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab_notrep (a int)");
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab_ins (a int)");
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab_full (a int)");
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab_full2 (x text)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rep (a int primary key)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_full_pk (a int primary key, b text)");
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_full_pk REPLICA IDENTITY FULL");
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab_nothing (a int)");

# different column count and order than on publisher
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_mixed (d text default 'local', c numeric, b text, a int primary key)"
);

# replication of the table with included index
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_include (a int, b text, CONSTRAINT covering PRIMARY KEY(a) INCLUDE(b))"
);

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres', "CREATE PUBLICATION tap_pub");
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_ins_only WITH (publish = insert)");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub ADD TABLE tab_rep, tab_full, tab_full2, tab_mixed, tab_include, tab_nothing, tab_full_pk"
);
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_ins_only ADD TABLE tab_ins");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub, tap_pub_ins_only"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

my $result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_notrep");
is($result, qq(0), 'check non-replicated table is empty on subscriber');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_ins");
is($result, qq(1002), 'check initial data was copied to subscriber');

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_ins SELECT generate_series(1,50)");
$node_publisher->safe_psql('postgres', "DELETE FROM tab_ins WHERE a > 20");
$node_publisher->safe_psql('postgres', "UPDATE tab_ins SET a = -a");

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rep SELECT generate_series(1,50)");
$node_publisher->safe_psql('postgres', "DELETE FROM tab_rep WHERE a > 20");
$node_publisher->safe_psql('postgres', "UPDATE tab_rep SET a = -a");

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_mixed VALUES (2, 'bar', 2.2)");

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_full_pk VALUES (1, 'foo'), (2, 'baz')");

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_nothing VALUES (generate_series(1,20))");

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_include SELECT generate_series(1,50)");
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab_include WHERE a > 20");
$node_publisher->safe_psql('postgres', "UPDATE tab_include SET a = -a");

$node_publisher->wait_for_catchup('tap_sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_ins");
is($result, qq(1052|1|1002), 'check replicated inserts on subscriber');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_rep");
is($result, qq(20|-20|-1), 'check replicated changes on subscriber');

$result = $node_subscriber->safe_psql('postgres', "SELECT * FROM tab_mixed");
is( $result, qq(local|1.1|foo|1
local|2.2|bar|2), 'check replicated changes with different column order');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_nothing");
is($result, qq(20), 'check replicated changes with REPLICA IDENTITY NOTHING');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_include");
is($result, qq(20|-20|-1),
	'check replicated changes with primary key index with included columns');

# insert some duplicate rows
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_full SELECT generate_series(1,10)");

# add REPLICA IDENTITY FULL so we can update
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_full REPLICA IDENTITY FULL");
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_full REPLICA IDENTITY FULL");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_full2 REPLICA IDENTITY FULL");
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_full2 REPLICA IDENTITY FULL");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_ins REPLICA IDENTITY FULL");
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_ins REPLICA IDENTITY FULL");
# tab_mixed can use DEFAULT, since it has a primary key

# and do the updates
$node_publisher->safe_psql('postgres', "UPDATE tab_full SET a = a * a");
$node_publisher->safe_psql('postgres',
	"UPDATE tab_full2 SET x = 'bb' WHERE x = 'b'");
$node_publisher->safe_psql('postgres',
	"UPDATE tab_mixed SET b = 'baz' WHERE a = 1");
$node_publisher->safe_psql('postgres',
	"UPDATE tab_full_pk SET b = 'bar' WHERE a = 1");

$node_publisher->wait_for_catchup('tap_sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_full");
is($result, qq(20|1|100),
	'update works with REPLICA IDENTITY FULL and duplicate tuples');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT x FROM tab_full2 ORDER BY 1");
is( $result, qq(a
bb
bb),
	'update works with REPLICA IDENTITY FULL and text datums');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM tab_mixed ORDER BY a");
is( $result, qq(local|1.1|baz|1
local|2.2|bar|2),
	'update works with different column order and subscriber local values');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM tab_full_pk ORDER BY a");
is( $result, qq(1|bar
2|baz),
	'update works with REPLICA IDENTITY FULL and a primary key');

# Check that subscriber handles cases where update/delete target tuple
# is missing.  We have to look for the DEBUG1 log messages about that,
# so temporarily bump up the log verbosity.
$node_subscriber->append_conf('postgresql.conf', "log_min_messages = debug1");
$node_subscriber->reload;

$node_subscriber->safe_psql('postgres', "DELETE FROM tab_full_pk");

# Note that the current location of the log file is not grabbed immediately
# after reloading the configuration, but after sending one SQL command to
# the node so as we are sure that the reloading has taken effect.
my $log_location = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
	"UPDATE tab_full_pk SET b = 'quux' WHERE a = 1");
$node_publisher->safe_psql('postgres', "DELETE FROM tab_full_pk WHERE a = 2");

$node_publisher->wait_for_catchup('tap_sub');

my $logfile = slurp_file($node_subscriber->logfile, $log_location);
ok( $logfile =~
	  qr/logical replication did not find row to be updated in replication target relation "tab_full_pk"/,
	'update target row is missing');
ok( $logfile =~
	  qr/logical replication did not find row to be deleted in replication target relation "tab_full_pk"/,
	'delete target row is missing');

$node_subscriber->append_conf('postgresql.conf',
	"log_min_messages = warning");
$node_subscriber->reload;

# check behavior with toasted values

$node_publisher->safe_psql('postgres',
	"UPDATE tab_mixed SET b = repeat('xyzzy', 100000) WHERE a = 2");

$node_publisher->wait_for_catchup('tap_sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT a, length(b), c, d FROM tab_mixed ORDER BY a");
is( $result, qq(1|3|1.1|local
2|500000|2.2|local),
	'update transmits large column value');

$node_publisher->safe_psql('postgres',
	"UPDATE tab_mixed SET c = 3.3 WHERE a = 2");

$node_publisher->wait_for_catchup('tap_sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT a, length(b), c, d FROM tab_mixed ORDER BY a");
is( $result, qq(1|3|1.1|local
2|500000|3.3|local),
	'update with non-transmitted large column value');

# check behavior with dropped columns

# this update should get transmitted before the column goes away
$node_publisher->safe_psql('postgres',
	"UPDATE tab_mixed SET b = 'bar', c = 2.2 WHERE a = 2");

$node_publisher->safe_psql('postgres', "ALTER TABLE tab_mixed DROP COLUMN b");

$node_publisher->safe_psql('postgres',
	"UPDATE tab_mixed SET c = 11.11 WHERE a = 1");

$node_publisher->wait_for_catchup('tap_sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM tab_mixed ORDER BY a");
is( $result, qq(local|11.11|baz|1
local|2.2|bar|2),
	'update works with dropped publisher column');

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_mixed DROP COLUMN d");

$node_publisher->safe_psql('postgres',
	"UPDATE tab_mixed SET c = 22.22 WHERE a = 2");

$node_publisher->wait_for_catchup('tap_sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM tab_mixed ORDER BY a");
is( $result, qq(11.11|baz|1
22.22|bar|2),
	'update works with dropped subscriber column');

# check that change of connection string and/or publication list causes
# restart of subscription workers. We check the state along with
# application_name to ensure that the walsender is (re)started.
#
# Not all of these are registered as tests as we need to poll for a change
# but the test suite will fail none the less when something goes wrong.
my $oldpid = $node_publisher->safe_psql('postgres',
	"SELECT pid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
);
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr sslmode=disable'"
);
$node_publisher->poll_query_until('postgres',
	"SELECT pid != $oldpid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
) or die "Timed out while waiting for apply to restart after changing CONNECTION";

$oldpid = $node_publisher->safe_psql('postgres',
	"SELECT pid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
);
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub SET PUBLICATION tap_pub_ins_only WITH (copy_data = false)"
);
$node_publisher->poll_query_until('postgres',
	"SELECT pid != $oldpid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
) or die "Timed out while waiting for apply to restart after changing PUBLICATION";

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_ins SELECT generate_series(1001,1100)");
$node_publisher->safe_psql('postgres', "DELETE FROM tab_rep");

# Restart the publisher and check the state of the subscriber which
# should be in a streaming state after catching up.
$node_publisher->stop('fast');
$node_publisher->start;

$node_publisher->wait_for_catchup('tap_sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_ins");
is($result, qq(1152|1|1100),
	'check replicated inserts after subscription publication change');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_rep");
is($result, qq(20|-20|-1),
	'check changes skipped after subscription publication change');

# check alter publication (relcache invalidation etc)
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_ins_only SET (publish = 'insert, delete')");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_ins_only ADD TABLE tab_full");
$node_publisher->safe_psql('postgres', "DELETE FROM tab_ins WHERE a > 0");
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH PUBLICATION WITH (copy_data = false)"
);
$node_publisher->safe_psql('postgres', "INSERT INTO tab_full VALUES(0)");

$node_publisher->wait_for_catchup('tap_sub');

# note that data are different on provider and subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_ins");
is($result, qq(1052|1|1002),
	'check replicated deletes after alter publication');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_full");
is($result, qq(21|0|100), 'check replicated insert after alter publication');

# check restart on rename
$oldpid = $node_publisher->safe_psql('postgres',
	"SELECT pid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
);
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub RENAME TO tap_sub_renamed");
$node_publisher->poll_query_until('postgres',
	"SELECT pid != $oldpid FROM pg_stat_replication WHERE application_name = 'tap_sub_renamed' AND state = 'streaming';"
) or die "Timed out while waiting for apply to restart after renaming SUBSCRIPTION";

# check all the cleanup
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_renamed");

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_subscription");
is($result, qq(0), 'check subscription was dropped on subscriber');

$result = $node_publisher->safe_psql('postgres',
	"SELECT count(*) FROM pg_replication_slots");
is($result, qq(0), 'check replication slot was dropped on publisher');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_subscription_rel");
is($result, qq(0),
	'check subscription relation status was dropped on subscriber');

$result = $node_publisher->safe_psql('postgres',
	"SELECT count(*) FROM pg_replication_slots");
is($result, qq(0), 'check replication slot was dropped on publisher');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_replication_origin");
is($result, qq(0), 'check replication origin was dropped on subscriber');

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

# CREATE PUBLICATION while wal_level=minimal should succeed, with a WARNING
$node_publisher->append_conf(
	'postgresql.conf', qq(
wal_level=minimal
max_wal_senders=0
));
$node_publisher->start;
($result, my $retout, my $reterr) = $node_publisher->psql(
	'postgres', qq{
BEGIN;
CREATE TABLE skip_wal();
CREATE PUBLICATION tap_pub2 FOR TABLE skip_wal;
ROLLBACK;
});
ok( $reterr =~
	  m/WARNING:  wal_level is insufficient to publish logical changes/,
	'CREATE PUBLICATION while wal_level=minimal');
