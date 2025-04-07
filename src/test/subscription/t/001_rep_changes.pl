
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Basic logical replication test
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
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

# Replicate the changes without replica identity index
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_no_replidentity_index(c1 int)");
$node_publisher->safe_psql('postgres',
	"CREATE INDEX idx_no_replidentity_index ON tab_no_replidentity_index(c1)"
);

# Replicate the changes without columns
$node_publisher->safe_psql('postgres', "CREATE TABLE tab_no_col()");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_no_col default VALUES");

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

# replication of the table without replica identity index
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_no_replidentity_index(c1 int)");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX idx_no_replidentity_index ON tab_no_replidentity_index(c1)"
);

# replication of the table without columns
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab_no_col()");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres', "CREATE PUBLICATION tap_pub");
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_ins_only WITH (publish = insert)");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub ADD TABLE tab_rep, tab_full, tab_full2, tab_mixed, tab_include, tab_nothing, tab_full_pk, tab_no_replidentity_index, tab_no_col"
);
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_ins_only ADD TABLE tab_ins");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub, tap_pub_ins_only"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Reset IO statistics, for the WAL sender check with pg_stat_io.
$node_publisher->safe_psql('postgres', "SELECT pg_stat_reset_shared('io')");

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

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_no_replidentity_index VALUES(1)");

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_no_col default VALUES");

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

is( $node_subscriber->safe_psql(
		'postgres', q(SELECT c1 FROM tab_no_replidentity_index)),
	1,
	"value replicated to subscriber without replica identity index");

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_no_col");
is($result, qq(2), 'check replicated changes for table having no columns');

# Wait for the logical WAL sender to update its IO statistics.  This is
# done before the next restart, which would force a flush of its stats, and
# far enough from the reset done above to not impact the run time.
$node_publisher->poll_query_until(
	'postgres',
	qq[SELECT sum(reads) > 0
       FROM pg_catalog.pg_stat_io
       WHERE backend_type = 'walsender'
       AND object = 'wal']
  )
  or die
  "Timed out while waiting for the walsender to update its IO statistics";

# insert some duplicate rows
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_full SELECT generate_series(1,10)");

# Test behaviour of ALTER PUBLICATION ... DROP TABLE
#
# When a publisher drops a table from publication, it should also stop sending
# its changes to subscribers. We look at the subscriber whether it receives
# the row that is inserted to the table on the publisher after it is dropped
# from the publication.
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_ins");
is($result, qq(1052|1|1002),
	'check rows on subscriber before table drop from publication');

# Drop the table from publication
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_ins_only DROP TABLE tab_ins");

# Insert a row in publisher, but publisher will not send this row to subscriber
$node_publisher->safe_psql('postgres', "INSERT INTO tab_ins VALUES(8888)");

$node_publisher->wait_for_catchup('tap_sub');

# Subscriber will not receive the inserted row, after table is dropped from
# publication, so row count should remain the same.
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_ins");
is($result, qq(1052|1|1002),
	'check rows on subscriber after table drop from publication');

# Delete the inserted row in publisher
$node_publisher->safe_psql('postgres', "DELETE FROM tab_ins WHERE a = 8888");

# Add the table to publication again
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_ins_only ADD TABLE tab_ins");

# Refresh publication after table is added to publication
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH PUBLICATION");

# Test replication with multiple publications for a subscription such that the
# operations are performed on the table from the first publication in the list.

# Create tables on publisher
$node_publisher->safe_psql('postgres', "CREATE TABLE temp1 (a int)");
$node_publisher->safe_psql('postgres', "CREATE TABLE temp2 (a int)");

# Create tables on subscriber
$node_subscriber->safe_psql('postgres', "CREATE TABLE temp1 (a int)");
$node_subscriber->safe_psql('postgres', "CREATE TABLE temp2 (a int)");

# Setup logical replication that will only be used for this test
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_temp1 FOR TABLE temp1 WITH (publish = insert)"
);
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_temp2 FOR TABLE temp2");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_temp1 CONNECTION '$publisher_connstr' PUBLICATION tap_pub_temp1, tap_pub_temp2"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher,
	'tap_sub_temp1');

# Subscriber table will have no rows initially
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM temp1");
is($result, qq(0),
	'check initial rows on subscriber with multiple publications');

# Insert a row into the table that's part of first publication in subscriber
# list of publications.
$node_publisher->safe_psql('postgres', "INSERT INTO temp1 VALUES (1)");

$node_publisher->wait_for_catchup('tap_sub_temp1');

# Subscriber should receive the inserted row
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM temp1");
is($result, qq(1), 'check rows on subscriber with multiple publications');

# Drop subscription as we don't need it anymore
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_temp1");

# Drop publications as we don't need them anymore
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_temp1");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_temp2");

# Clean up the tables on both publisher and subscriber as we don't need them
$node_publisher->safe_psql('postgres', "DROP TABLE temp1");
$node_publisher->safe_psql('postgres', "DROP TABLE temp2");
$node_subscriber->safe_psql('postgres', "DROP TABLE temp1");
$node_subscriber->safe_psql('postgres', "DROP TABLE temp2");

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

$node_subscriber->safe_psql('postgres', "DELETE FROM tab_full_pk");
$node_subscriber->safe_psql('postgres', "DELETE FROM tab_full WHERE a = 25");

# Note that the current location of the log file is not grabbed immediately
# after reloading the configuration, but after sending one SQL command to
# the node so as we are sure that the reloading has taken effect.
my $log_location = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
	"UPDATE tab_full_pk SET b = 'quux' WHERE a = 1");
$node_publisher->safe_psql('postgres',
	"UPDATE tab_full SET a = a + 1 WHERE a = 25");
$node_publisher->safe_psql('postgres', "DELETE FROM tab_full_pk WHERE a = 2");

$node_publisher->wait_for_catchup('tap_sub');

my $logfile = slurp_file($node_subscriber->logfile, $log_location);
ok( $logfile =~
	  qr/conflict detected on relation "public.tab_full_pk": conflict=update_missing.*\n.*DETAIL:.* Could not find the row to be updated.*\n.*Remote tuple \(1, quux\); replica identity \(a\)=\(1\)/m,
	'update target row is missing');
ok( $logfile =~
	  qr/conflict detected on relation "public.tab_full": conflict=update_missing.*\n.*DETAIL:.* Could not find the row to be updated.*\n.*Remote tuple \(26\); replica identity full \(25\)/m,
	'update target row is missing');
ok( $logfile =~
	  qr/conflict detected on relation "public.tab_full_pk": conflict=delete_missing.*\n.*DETAIL:.* Could not find the row to be deleted.*\n.*Replica identity \(a\)=\(2\)/m,
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
# but the test suite will fail nonetheless when something goes wrong.
my $oldpid = $node_publisher->safe_psql('postgres',
	"SELECT pid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
);
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr sslmode=disable'"
);
$node_publisher->poll_query_until('postgres',
	"SELECT pid != $oldpid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
  )
  or die
  "Timed out while waiting for apply to restart after changing CONNECTION";

$oldpid = $node_publisher->safe_psql('postgres',
	"SELECT pid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
);
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub SET PUBLICATION tap_pub_ins_only WITH (copy_data = false)"
);
$node_publisher->poll_query_until('postgres',
	"SELECT pid != $oldpid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
  )
  or die
  "Timed out while waiting for apply to restart after changing PUBLICATION";

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

# Check that we don't send BEGIN and COMMIT because of empty transaction
# optimization.  We have to look for the DEBUG1 log messages about that, so
# temporarily bump up the log verbosity.
$node_publisher->append_conf('postgresql.conf', "log_min_messages = debug1");
$node_publisher->reload;

# Note that the current location of the log file is not grabbed immediately
# after reloading the configuration, but after sending one SQL command to
# the node so that we are sure that the reloading has taken effect.
$log_location = -s $node_publisher->logfile;

$node_publisher->safe_psql('postgres', "INSERT INTO tab_notrep VALUES (11)");

$node_publisher->wait_for_catchup('tap_sub');

$logfile = slurp_file($node_publisher->logfile, $log_location);
ok($logfile =~ qr/skipped replication of an empty transaction with XID/,
	'empty transaction is skipped');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_notrep");
is($result, qq(0), 'check non-replicated table is empty on subscriber');

$node_publisher->append_conf('postgresql.conf', "log_min_messages = warning");
$node_publisher->reload;

# note that data are different on provider and subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_ins");
is($result, qq(1052|1|1002),
	'check replicated deletes after alter publication');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_full");
is($result, qq(19|0|100), 'check replicated insert after alter publication');

# check restart on rename
$oldpid = $node_publisher->safe_psql('postgres',
	"SELECT pid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
);
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub RENAME TO tap_sub_renamed");
$node_publisher->poll_query_until('postgres',
	"SELECT pid != $oldpid FROM pg_stat_replication WHERE application_name = 'tap_sub_renamed' AND state = 'streaming';"
  )
  or die
  "Timed out while waiting for apply to restart after renaming SUBSCRIPTION";

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
	  m/WARNING:  "wal_level" is insufficient to publish logical changes/,
	'CREATE PUBLICATION while "wal_level=minimal"');

done_testing();
