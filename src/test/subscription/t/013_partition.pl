
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test logical replication with partitioned tables
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# setup

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber1 = PostgreSQL::Test::Cluster->new('subscriber1');
$node_subscriber1->init;
$node_subscriber1->start;

my $node_subscriber2 = PostgreSQL::Test::Cluster->new('subscriber2');
$node_subscriber2->init;
$node_subscriber2->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

# publisher
$node_publisher->safe_psql('postgres', "CREATE PUBLICATION pub1");
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub_all FOR ALL TABLES");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, b text) PARTITION BY LIST (a)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1_1 (b text, a int NOT NULL)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab1 ATTACH PARTITION tab1_1 FOR VALUES IN (1, 2, 3)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1_2 PARTITION OF tab1 FOR VALUES IN (4, 5, 6)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1_def PARTITION OF tab1 DEFAULT");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION pub1 ADD TABLE tab1, tab1_1");

# subscriber1
#
# This is partitioned differently from the publisher.  tab1_2 is
# subpartitioned.  This tests the tuple routing code on the
# subscriber.
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1 (c text, a int PRIMARY KEY, b text) PARTITION BY LIST (a)"
);
# make a BRIN index to test aminsertcleanup logic in subscriber
$node_subscriber1->safe_psql('postgres',
	"CREATE INDEX tab1_c_brin_idx ON tab1 USING brin (c)"
);
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_1 (b text, c text DEFAULT 'sub1_tab1', a int NOT NULL)"
);
$node_subscriber1->safe_psql('postgres',
	"ALTER TABLE tab1 ATTACH PARTITION tab1_1 FOR VALUES IN (1, 2, 3)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_2 PARTITION OF tab1 (c DEFAULT 'sub1_tab1') FOR VALUES IN (4, 5, 6) PARTITION BY LIST (a)"
);
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_2_1 (c text, b text, a int NOT NULL)");
$node_subscriber1->safe_psql('postgres',
	"ALTER TABLE tab1_2 ATTACH PARTITION tab1_2_1 FOR VALUES IN (5)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_2_2 PARTITION OF tab1_2 FOR VALUES IN (4, 6)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_def PARTITION OF tab1 (c DEFAULT 'sub1_tab1') DEFAULT"
);
$node_subscriber1->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);

# Add set of AFTER replica triggers for testing that they are fired
# correctly.  This uses a table that records details of all trigger
# activities.  Triggers are marked as enabled for a subset of the
# partition tree.
$node_subscriber1->safe_psql(
	'postgres', qq{
CREATE TABLE sub1_trigger_activity (tgtab text, tgop text,
  tgwhen text, tglevel text, olda int, newa int);
CREATE FUNCTION sub1_trigger_activity_func() RETURNS TRIGGER AS \$\$
BEGIN
  IF (TG_OP = 'INSERT') THEN
    INSERT INTO public.sub1_trigger_activity
      SELECT TG_RELNAME, TG_OP, TG_WHEN, TG_LEVEL, NULL, NEW.a;
  ELSIF (TG_OP = 'UPDATE') THEN
    INSERT INTO public.sub1_trigger_activity
      SELECT TG_RELNAME, TG_OP, TG_WHEN, TG_LEVEL, OLD.a, NEW.a;
  END IF;
  RETURN NULL;
END;
\$\$ LANGUAGE plpgsql;
CREATE TRIGGER sub1_tab1_log_op_trigger
  AFTER INSERT OR UPDATE ON tab1
  FOR EACH ROW EXECUTE PROCEDURE sub1_trigger_activity_func();
ALTER TABLE ONLY tab1 ENABLE REPLICA TRIGGER sub1_tab1_log_op_trigger;
CREATE TRIGGER sub1_tab1_2_log_op_trigger
  AFTER INSERT OR UPDATE ON tab1_2
  FOR EACH ROW EXECUTE PROCEDURE sub1_trigger_activity_func();
ALTER TABLE ONLY tab1_2 ENABLE REPLICA TRIGGER sub1_tab1_2_log_op_trigger;
CREATE TRIGGER sub1_tab1_2_2_log_op_trigger
  AFTER INSERT OR UPDATE ON tab1_2_2
  FOR EACH ROW EXECUTE PROCEDURE sub1_trigger_activity_func();
ALTER TABLE ONLY tab1_2_2 ENABLE REPLICA TRIGGER sub1_tab1_2_2_log_op_trigger;
});

# subscriber 2
#
# This does not use partitioning.  The tables match the leaf tables on
# the publisher.
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1', b text)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1_1', b text)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_2 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1_2', b text)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_def (a int PRIMARY KEY, b text, c text DEFAULT 'sub2_tab1_def')"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub2 CONNECTION '$publisher_connstr' PUBLICATION pub_all"
);

# Add set of AFTER replica triggers for testing that they are fired
# correctly, using the same method as the first subscriber.
$node_subscriber2->safe_psql(
	'postgres', qq{
CREATE TABLE sub2_trigger_activity (tgtab text,
  tgop text, tgwhen text, tglevel text, olda int, newa int);
CREATE FUNCTION sub2_trigger_activity_func() RETURNS TRIGGER AS \$\$
BEGIN
  IF (TG_OP = 'INSERT') THEN
    INSERT INTO public.sub2_trigger_activity
      SELECT TG_RELNAME, TG_OP, TG_WHEN, TG_LEVEL, NULL, NEW.a;
  ELSIF (TG_OP = 'UPDATE') THEN
    INSERT INTO public.sub2_trigger_activity
      SELECT TG_RELNAME, TG_OP, TG_WHEN, TG_LEVEL, OLD.a, NEW.a;
  END IF;
  RETURN NULL;
END;
\$\$ LANGUAGE plpgsql;
CREATE TRIGGER sub2_tab1_log_op_trigger
  AFTER INSERT OR UPDATE ON tab1
  FOR EACH ROW EXECUTE PROCEDURE sub2_trigger_activity_func();
ALTER TABLE ONLY tab1 ENABLE REPLICA TRIGGER sub2_tab1_log_op_trigger;
CREATE TRIGGER sub2_tab1_2_log_op_trigger
  AFTER INSERT OR UPDATE ON tab1_2
  FOR EACH ROW EXECUTE PROCEDURE sub2_trigger_activity_func();
ALTER TABLE ONLY tab1_2 ENABLE REPLICA TRIGGER sub2_tab1_2_log_op_trigger;
});

# Wait for initial sync of all subscriptions
$node_subscriber1->wait_for_subscription_sync;
$node_subscriber2->wait_for_subscription_sync;

# Tests for replication using leaf partition identity and schema

# insert
$node_publisher->safe_psql('postgres', "INSERT INTO tab1 VALUES (1)");
$node_publisher->safe_psql('postgres', "INSERT INTO tab1_1 (a) VALUES (3)");
$node_publisher->safe_psql('postgres', "INSERT INTO tab1_2 VALUES (5)");
$node_publisher->safe_psql('postgres', "INSERT INTO tab1 VALUES (0)");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

my $result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub1_tab1|0
sub1_tab1|1
sub1_tab1|3
sub1_tab1|5), 'inserts into tab1 and its partitions replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1_2_1 ORDER BY 1");
is($result, qq(5), 'inserts into tab1_2 replicated into tab1_2_1 correctly');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1_2_2 ORDER BY 1");
is($result, qq(), 'inserts into tab1_2 replicated into tab1_2_2 correctly');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1_1|1
sub2_tab1_1|3), 'inserts into tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_2 ORDER BY 1, 2");
is($result, qq(sub2_tab1_2|5), 'inserts into tab1_2 replicated');

# The AFTER trigger of tab1_2 should have recorded one INSERT.
$result = $node_subscriber2->safe_psql('postgres',
	"SELECT * FROM sub2_trigger_activity ORDER BY tgtab, tgop, tgwhen, olda, newa;"
);
is( $result,
	qq(tab1_2|INSERT|AFTER|ROW||5),
	'check replica insert after trigger applied on subscriber');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_def ORDER BY 1, 2");
is($result, qq(sub2_tab1_def|0), 'inserts into tab1_def replicated');

# update (replicated as update)
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 2 WHERE a = 1");
# All of the following cause an update to be applied to a partitioned
# table on subscriber1: tab1_2 is leaf partition on publisher, whereas
# it's sub-partitioned on subscriber1.
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 6 WHERE a = 5");
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 4 WHERE a = 6");
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 6 WHERE a = 4");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub1_tab1|0
sub1_tab1|2
sub1_tab1|3
sub1_tab1|6), 'update of tab1_1, tab1_2 replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1_2_1 ORDER BY 1");
is($result, qq(), 'updates of tab1_2 replicated into tab1_2_1 correctly');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1_2_2 ORDER BY 1");
is($result, qq(6), 'updates of tab1_2 replicated into tab1_2_2 correctly');

# The AFTER trigger should have recorded the UPDATEs of tab1_2_2.
$result = $node_subscriber1->safe_psql('postgres',
	"SELECT * FROM sub1_trigger_activity ORDER BY tgtab, tgop, tgwhen, olda, newa;"
);
is( $result, qq(tab1_2_2|INSERT|AFTER|ROW||6
tab1_2_2|UPDATE|AFTER|ROW|4|6
tab1_2_2|UPDATE|AFTER|ROW|6|4),
	'check replica update after trigger applied on subscriber');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1_1|2
sub2_tab1_1|3), 'update of tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_2 ORDER BY 1, 2");
is($result, qq(sub2_tab1_2|6), 'tab1_2 updated');

# The AFTER trigger should have recorded the updates of tab1_2.
$result = $node_subscriber2->safe_psql('postgres',
	"SELECT * FROM sub2_trigger_activity ORDER BY tgtab, tgop, tgwhen, olda, newa;"
);
is( $result, qq(tab1_2|INSERT|AFTER|ROW||5
tab1_2|UPDATE|AFTER|ROW|4|6
tab1_2|UPDATE|AFTER|ROW|5|6
tab1_2|UPDATE|AFTER|ROW|6|4),
	'check replica update after trigger applied on subscriber');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_def ORDER BY 1");
is($result, qq(sub2_tab1_def|0), 'tab1_def unchanged');

# update (replicated as delete+insert)
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 1 WHERE a = 0");
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 4 WHERE a = 1");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub1_tab1|2
sub1_tab1|3
sub1_tab1|4
sub1_tab1|6),
	'update of tab1 (delete from tab1_def + insert into tab1_1) replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1_2_2 ORDER BY 1");
is( $result, qq(4
6), 'updates of tab1 (delete + insert) replicated into tab1_2_2 correctly');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1_1|2
sub2_tab1_1|3), 'tab1_1 unchanged');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_2 ORDER BY 1, 2");
is( $result, qq(sub2_tab1_2|4
sub2_tab1_2|6), 'insert into tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT a FROM tab1_def ORDER BY 1");
is($result, qq(), 'delete from tab1_def replicated');

# delete
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab1 WHERE a IN (2, 3, 5)");
$node_publisher->safe_psql('postgres', "DELETE FROM tab1_2");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab1");
is($result, qq(), 'delete from tab1_1, tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1_1");
is($result, qq(), 'delete from tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1_2");
is($result, qq(), 'delete from tab1_2 replicated');

# truncate
$node_subscriber1->safe_psql('postgres',
	"INSERT INTO tab1 (a) VALUES (1), (2), (5)");
$node_subscriber2->safe_psql('postgres', "INSERT INTO tab1_2 (a) VALUES (2)");
$node_publisher->safe_psql('postgres', "TRUNCATE tab1_2");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result =
  $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab1 ORDER BY 1");
is( $result, qq(1
2), 'truncate of tab1_2 replicated');

$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1_2 ORDER BY 1");
is($result, qq(), 'truncate of tab1_2 replicated');

$node_publisher->safe_psql('postgres', "TRUNCATE tab1");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result =
  $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab1 ORDER BY 1");
is($result, qq(), 'truncate of tab1_1 replicated');
$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1 ORDER BY 1");
is($result, qq(), 'truncate of tab1 replicated');

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab1 VALUES (1, 'foo'), (4, 'bar'), (10, 'baz')");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$node_subscriber1->safe_psql('postgres', "DELETE FROM tab1");

# Note that the current location of the log file is not grabbed immediately
# after reloading the configuration, but after sending one SQL command to
# the node so as we are sure that the reloading has taken effect.
my $log_location = -s $node_subscriber1->logfile;

$node_publisher->safe_psql('postgres',
	"UPDATE tab1 SET b = 'quux' WHERE a = 4");
$node_publisher->safe_psql('postgres', "DELETE FROM tab1");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

my $logfile = slurp_file($node_subscriber1->logfile(), $log_location);
ok( $logfile =~
	  qr/conflict detected on relation "public.tab1_2_2": conflict=update_missing.*\n.*DETAIL:.* Could not find the row to be updated.*\n.*Remote tuple \(null, 4, quux\); replica identity \(a\)=\(4\)/,
	'update target row is missing in tab1_2_2');
ok( $logfile =~
	  qr/conflict detected on relation "public.tab1_1": conflict=delete_missing.*\n.*DETAIL:.* Could not find the row to be deleted.*\n.*Replica identity \(a\)=\(1\)/,
	'delete target row is missing in tab1_1');
ok( $logfile =~
	  qr/conflict detected on relation "public.tab1_2_2": conflict=delete_missing.*\n.*DETAIL:.* Could not find the row to be deleted.*\n.*Replica identity \(a\)=\(4\)/,
	'delete target row is missing in tab1_2_2');
ok( $logfile =~
	  qr/conflict detected on relation "public.tab1_def": conflict=delete_missing.*\n.*DETAIL:.* Could not find the row to be deleted.*\n.*Replica identity \(a\)=\(10\)/,
	'delete target row is missing in tab1_def');

# Tests for replication using root table identity and schema

# publisher
$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub1");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab2 (a int PRIMARY KEY, b text) PARTITION BY LIST (a)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab2_1 (b text, a int NOT NULL)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab2 ATTACH PARTITION tab2_1 FOR VALUES IN (0, 1, 2, 3)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab2_2 PARTITION OF tab2 FOR VALUES IN (5, 6)");

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab3 (a int PRIMARY KEY, b text) PARTITION BY LIST (a)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab3_1 PARTITION OF tab3 FOR VALUES IN (0, 1, 2, 3, 5, 6)");

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab4 (a int PRIMARY KEY) PARTITION BY LIST (a)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab4_1 PARTITION OF tab4 FOR VALUES IN (-1, 0, 1) PARTITION BY LIST (a)"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab4_1_1 PARTITION OF tab4_1 FOR VALUES IN (-1, 0, 1)");

$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION pub_all SET (publish_via_partition_root = true)");
# Note: tab3_1's parent is not in the publication, in which case its
# changes are published using own identity. For tab2, even though both parent
# and child tables are present but changes will be replicated via the parent's
# identity and only once.
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub_viaroot FOR TABLE tab2, tab2_1, tab3_1 WITH (publish_via_partition_root = true)"
);

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub_lower_level FOR TABLE tab4_1 WITH (publish_via_partition_root = true)"
);

# prepare data for the initial sync
$node_publisher->safe_psql('postgres', "INSERT INTO tab2 VALUES (1)");
$node_publisher->safe_psql('postgres', "INSERT INTO tab4 VALUES (-1)");

# subscriber 1
$node_subscriber1->safe_psql('postgres', "DROP SUBSCRIPTION sub1");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab2 (a int PRIMARY KEY, c text DEFAULT 'sub1_tab2', b text) PARTITION BY RANGE (a)"
);
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab2_1 (c text DEFAULT 'sub1_tab2', b text, a int NOT NULL)"
);
$node_subscriber1->safe_psql('postgres',
	"ALTER TABLE tab2 ATTACH PARTITION tab2_1 FOR VALUES FROM (0) TO (10)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab3_1 (c text DEFAULT 'sub1_tab3_1', b text, a int NOT NULL PRIMARY KEY)"
);
$node_subscriber1->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub_viaroot CONNECTION '$publisher_connstr' PUBLICATION pub_viaroot"
);

# subscriber 2
$node_subscriber2->safe_psql('postgres', "DROP TABLE tab1");
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1', b text) PARTITION BY HASH (a)"
);
# Note: tab1's partitions are named tab1_1 and tab1_2 on the publisher.
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_part1 (b text, c text, a int NOT NULL)");
$node_subscriber2->safe_psql('postgres',
	"ALTER TABLE tab1 ATTACH PARTITION tab1_part1 FOR VALUES WITH (MODULUS 2, REMAINDER 0)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_part2 PARTITION OF tab1 FOR VALUES WITH (MODULUS 2, REMAINDER 1)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab2 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab2', b text)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab3 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab3', b text)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab3_1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab3_1', b text)"
);

# Note: We create two separate tables, not a partitioned one, so that we can
# easily identity through which relation were the changes replicated.
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab4 (a int PRIMARY KEY)");
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab4_1 (a int PRIMARY KEY)");
# Since we specified publish_via_partition_root in pub_all and
# pub_lower_level, all partition tables use their root tables' identity and
# schema. We set the list of publications so that the FOR ALL TABLES
# publication is second (the list order matters).
$node_subscriber2->safe_psql('postgres',
	"ALTER SUBSCRIPTION sub2 SET PUBLICATION pub_lower_level, pub_all");

# Wait for initial sync of all subscriptions
$node_subscriber1->wait_for_subscription_sync;
$node_subscriber2->wait_for_subscription_sync;

# check that data is synced correctly
$result = $node_subscriber1->safe_psql('postgres', "SELECT c, a FROM tab2");
is($result, qq(sub1_tab2|1), 'initial data synced for pub_viaroot');
$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab4 ORDER BY 1");
is($result, qq(-1), 'initial data synced for pub_lower_level and pub_all');
$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab4_1 ORDER BY 1");
is($result, qq(), 'initial data synced for pub_lower_level and pub_all');

# insert
$node_publisher->safe_psql('postgres', "INSERT INTO tab1 VALUES (1), (0)");
$node_publisher->safe_psql('postgres', "INSERT INTO tab1_1 (a) VALUES (3)");
$node_publisher->safe_psql('postgres', "INSERT INTO tab1_2 VALUES (5)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab2 VALUES (0), (3), (5)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab3 VALUES (1), (0), (3), (5)");

# Insert a row into the leaf partition, should be replicated through the
# partition root (thanks to the FOR ALL TABLES partition).
$node_publisher->safe_psql('postgres', "INSERT INTO tab4 VALUES (0)");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub1_tab2|0
sub1_tab2|1
sub1_tab2|3
sub1_tab2|5), 'inserts into tab2 replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab3_1 ORDER BY 1, 2");
is( $result, qq(sub1_tab3_1|0
sub1_tab3_1|1
sub1_tab3_1|3
sub1_tab3_1|5), 'inserts into tab3_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1|0
sub2_tab1|1
sub2_tab1|3
sub2_tab1|5), 'inserts into tab1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub2_tab2|0
sub2_tab2|1
sub2_tab2|3
sub2_tab2|5), 'inserts into tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab3 ORDER BY 1, 2");
is( $result, qq(sub2_tab3|0
sub2_tab3|1
sub2_tab3|3
sub2_tab3|5), 'inserts into tab3 replicated');

# tab4 change should be replicated through the root partition, which
# maps to the tab4 relation on subscriber.
$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab4 ORDER BY 1");
is( $result, qq(-1
0), 'inserts into tab4 replicated');

$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab4_1 ORDER BY 1");
is($result, qq(), 'inserts into tab4_1 replicated');


# now switch the order of publications in the list, try again, the result
# should be the same (no dependence on order of publications)
$node_subscriber2->safe_psql('postgres',
	"ALTER SUBSCRIPTION sub2 SET PUBLICATION pub_all, pub_lower_level");

# make sure the subscription on the second subscriber is synced, before
# continuing
$node_subscriber2->wait_for_subscription_sync;

# Insert a change into the leaf partition, should be replicated through
# the partition root (thanks to the FOR ALL TABLES partition).
$node_publisher->safe_psql('postgres', "INSERT INTO tab4 VALUES (1)");

$node_publisher->wait_for_catchup('sub2');

# tab4 change should be replicated through the root partition, which
# maps to the tab4 relation on subscriber.
$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab4 ORDER BY 1");
is( $result, qq(-1
0
1), 'inserts into tab4 replicated');

$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab4_1 ORDER BY 1");
is($result, qq(), 'inserts into tab4_1 replicated');


# update (replicated as update)
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 6 WHERE a = 5");
$node_publisher->safe_psql('postgres', "UPDATE tab2 SET a = 6 WHERE a = 5");
$node_publisher->safe_psql('postgres', "UPDATE tab3 SET a = 6 WHERE a = 5");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub1_tab2|0
sub1_tab2|1
sub1_tab2|3
sub1_tab2|6), 'update of tab2 replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab3_1 ORDER BY 1, 2");
is( $result, qq(sub1_tab3_1|0
sub1_tab3_1|1
sub1_tab3_1|3
sub1_tab3_1|6), 'update of tab3_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1|0
sub2_tab1|1
sub2_tab1|3
sub2_tab1|6), 'inserts into tab1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub2_tab2|0
sub2_tab2|1
sub2_tab2|3
sub2_tab2|6), 'inserts into tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab3 ORDER BY 1, 2");
is( $result, qq(sub2_tab3|0
sub2_tab3|1
sub2_tab3|3
sub2_tab3|6), 'inserts into tab3 replicated');

# update (replicated as delete+insert)
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 2 WHERE a = 6");
$node_publisher->safe_psql('postgres', "UPDATE tab2 SET a = 2 WHERE a = 6");
$node_publisher->safe_psql('postgres', "UPDATE tab3 SET a = 2 WHERE a = 6");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub1_tab2|0
sub1_tab2|1
sub1_tab2|2
sub1_tab2|3), 'update of tab2 replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab3_1 ORDER BY 1, 2");
is( $result, qq(sub1_tab3_1|0
sub1_tab3_1|1
sub1_tab3_1|2
sub1_tab3_1|3), 'update of tab3_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1|0
sub2_tab1|1
sub2_tab1|2
sub2_tab1|3), 'update of tab1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub2_tab2|0
sub2_tab2|1
sub2_tab2|2
sub2_tab2|3), 'update of tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab3 ORDER BY 1, 2");
is( $result, qq(sub2_tab3|0
sub2_tab3|1
sub2_tab3|2
sub2_tab3|3), 'update of tab3 replicated');

# delete
$node_publisher->safe_psql('postgres', "DELETE FROM tab1");
$node_publisher->safe_psql('postgres', "DELETE FROM tab2");
$node_publisher->safe_psql('postgres', "DELETE FROM tab3");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab2");
is($result, qq(), 'delete tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1");
is($result, qq(), 'delete from tab1 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab2");
is($result, qq(), 'delete from tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab3");
is($result, qq(), 'delete from tab3 replicated');

# truncate
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab1 VALUES (1), (2), (5)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab2 VALUES (1), (2), (5)");
# these will NOT be replicated
$node_publisher->safe_psql('postgres', "TRUNCATE tab1_2, tab2_1, tab3_1");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result =
  $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab2 ORDER BY 1");
is( $result, qq(1
2
5), 'truncate of tab2_1 NOT replicated');

$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1 ORDER BY 1");
is( $result, qq(1
2
5), 'truncate of tab1_2 NOT replicated');

$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab2 ORDER BY 1");
is( $result, qq(1
2
5), 'truncate of tab2_1 NOT replicated');

$node_publisher->safe_psql('postgres', "TRUNCATE tab1, tab2, tab3");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab2");
is($result, qq(), 'truncate of tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1");
is($result, qq(), 'truncate of tab1 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab2");
is($result, qq(), 'truncate of tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab3");
is($result, qq(), 'truncate of tab3 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab3_1");
is($result, qq(), 'truncate of tab3_1 replicated');

# check that the map to convert tuples from leaf partition to the root
# table is correctly rebuilt when a new column is added
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab2 DROP b, ADD COLUMN c text DEFAULT 'pub_tab2', ADD b text"
);
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab2 (a, b) VALUES (1, 'xxx'), (3, 'yyy'), (5, 'zzz')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab2 (a, b, c) VALUES (6, 'aaa', 'xxx_c')");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a, b FROM tab2 ORDER BY 1, 2");
is( $result, qq(pub_tab2|1|xxx
pub_tab2|3|yyy
pub_tab2|5|zzz
xxx_c|6|aaa), 'inserts into tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a, b FROM tab2 ORDER BY 1, 2");
is( $result, qq(pub_tab2|1|xxx
pub_tab2|3|yyy
pub_tab2|5|zzz
xxx_c|6|aaa), 'inserts into tab2 replicated');

$node_subscriber1->safe_psql('postgres', "DELETE FROM tab2");

# Note that the current location of the log file is not grabbed immediately
# after reloading the configuration, but after sending one SQL command to
# the node so as we are sure that the reloading has taken effect.
$log_location = -s $node_subscriber1->logfile;

$node_publisher->safe_psql('postgres',
	"UPDATE tab2 SET b = 'quux' WHERE a = 5");
$node_publisher->safe_psql('postgres', "DELETE FROM tab2 WHERE a = 1");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$logfile = slurp_file($node_subscriber1->logfile(), $log_location);
ok( $logfile =~
	  qr/conflict detected on relation "public.tab2_1": conflict=update_missing.*\n.*DETAIL:.* Could not find the row to be updated.*\n.*Remote tuple \(pub_tab2, quux, 5\); replica identity \(a\)=\(5\)/,
	'update target row is missing in tab2_1');
ok( $logfile =~
	  qr/conflict detected on relation "public.tab2_1": conflict=delete_missing.*\n.*DETAIL:.* Could not find the row to be deleted.*\n.*Replica identity \(a\)=\(1\)/,
	'delete target row is missing in tab2_1');

# Enable the track_commit_timestamp to detect the conflict when attempting
# to update a row that was previously modified by a different origin.
$node_subscriber1->append_conf('postgresql.conf',
	'track_commit_timestamp = on');
$node_subscriber1->restart;

$node_subscriber1->safe_psql('postgres',
	"INSERT INTO tab2 VALUES (3, 'yyy')");
$node_publisher->safe_psql('postgres',
	"UPDATE tab2 SET b = 'quux' WHERE a = 3");

$node_publisher->wait_for_catchup('sub_viaroot');

$logfile = slurp_file($node_subscriber1->logfile(), $log_location);
ok( $logfile =~
	  qr/conflict detected on relation "public.tab2_1": conflict=update_origin_differs.*\n.*DETAIL:.* Updating the row that was modified locally in transaction [0-9]+ at .*\n.*Existing local tuple \(yyy, null, 3\); remote tuple \(pub_tab2, quux, 3\); replica identity \(a\)=\(3\)/,
	'updating a tuple that was modified by a different origin');

# The remaining tests no longer test conflict detection.
$node_subscriber1->append_conf('postgresql.conf',
	'track_commit_timestamp = off');
$node_subscriber1->restart;

# Test that replication continues to work correctly after altering the
# partition of a partitioned target table.

$node_publisher->safe_psql(
	'postgres', q{
	CREATE TABLE tab5 (a int NOT NULL, b int);
	CREATE UNIQUE INDEX tab5_a_idx ON tab5 (a);
	ALTER TABLE tab5 REPLICA IDENTITY USING INDEX tab5_a_idx;});

$node_subscriber2->safe_psql(
	'postgres', q{
	CREATE TABLE tab5 (a int NOT NULL, b int, c int) PARTITION BY LIST (a);
	CREATE TABLE tab5_1 PARTITION OF tab5 DEFAULT;
	CREATE UNIQUE INDEX tab5_a_idx ON tab5 (a);
	ALTER TABLE tab5 REPLICA IDENTITY USING INDEX tab5_a_idx;
	ALTER TABLE tab5_1 REPLICA IDENTITY USING INDEX tab5_1_a_idx;});

$node_subscriber2->safe_psql('postgres',
	"ALTER SUBSCRIPTION sub2 REFRESH PUBLICATION");

$node_subscriber2->wait_for_subscription_sync;

# Make partition map cache
$node_publisher->safe_psql('postgres', "INSERT INTO tab5 VALUES (1, 1)");
$node_publisher->safe_psql('postgres', "UPDATE tab5 SET a = 2 WHERE a = 1");

$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT a, b FROM tab5 ORDER BY 1");
is($result, qq(2|1), 'updates of tab5 replicated correctly');

# Change the column order of partition on subscriber
$node_subscriber2->safe_psql(
	'postgres', q{
	ALTER TABLE tab5 DETACH PARTITION tab5_1;
	ALTER TABLE tab5_1 DROP COLUMN b;
	ALTER TABLE tab5_1 ADD COLUMN b int;
	ALTER TABLE tab5 ATTACH PARTITION tab5_1 DEFAULT});

$node_publisher->safe_psql('postgres', "UPDATE tab5 SET a = 3 WHERE a = 2");

$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT a, b, c FROM tab5 ORDER BY 1");
is($result, qq(3|1|),
	'updates of tab5 replicated correctly after altering table on subscriber'
);

# Test that replication into the partitioned target table continues to
# work correctly when the published table is altered.
$node_publisher->safe_psql(
	'postgres', q{
	ALTER TABLE tab5 DROP COLUMN b, ADD COLUMN c INT;
	ALTER TABLE tab5 ADD COLUMN b INT;});

$node_publisher->safe_psql('postgres', "UPDATE tab5 SET c = 1 WHERE a = 3");

$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT a, b, c FROM tab5 ORDER BY 1");
is($result, qq(3||1),
	'updates of tab5 replicated correctly after altering table on publisher');

# Test that replication works correctly as long as the leaf partition
# has the necessary REPLICA IDENTITY, even though the actual target
# partitioned table does not.
$node_subscriber2->safe_psql('postgres',
	"ALTER TABLE tab5 REPLICA IDENTITY NOTHING");

$node_publisher->safe_psql('postgres', "UPDATE tab5 SET a = 4 WHERE a = 3");

$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT a, b, c FROM tab5_1 ORDER BY 1");
is($result, qq(4||1), 'updates of tab5 replicated correctly');

done_testing();
