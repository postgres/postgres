
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test the CREATE SUBSCRIPTION 'origin' parameter and its interaction with
# 'copy_data' parameter.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $subname_AB = 'tap_sub_A_B';
my $subname_AB2 = 'tap_sub_A_B_2';
my $subname_BA = 'tap_sub_B_A';
my $subname_BC = 'tap_sub_B_C';

my $result;
my $stdout;
my $stderr;

###############################################################################
# Setup a bidirectional logical replication between node_A & node_B
###############################################################################

# Initialize nodes
# node_A
my $node_A = PostgreSQL::Test::Cluster->new('node_A');
$node_A->init(allows_streaming => 'logical');
$node_A->start;

# node_B
my $node_B = PostgreSQL::Test::Cluster->new('node_B');
$node_B->init(allows_streaming => 'logical');

# Enable the track_commit_timestamp to detect the conflict when attempting to
# update a row that was previously modified by a different origin.
$node_B->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$node_B->start;

# Create table on node_A
$node_A->safe_psql('postgres', "CREATE TABLE tab (a int PRIMARY KEY)");

# Create the same table on node_B
$node_B->safe_psql('postgres', "CREATE TABLE tab (a int PRIMARY KEY)");

# Setup logical replication
# node_A (pub) -> node_B (sub)
my $node_A_connstr = $node_A->connstr . ' dbname=postgres';
$node_A->safe_psql('postgres', "CREATE PUBLICATION tap_pub_A FOR TABLE tab");
$node_B->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION $subname_BA
	CONNECTION '$node_A_connstr application_name=$subname_BA'
	PUBLICATION tap_pub_A
	WITH (origin = none)");

# node_B (pub) -> node_A (sub)
my $node_B_connstr = $node_B->connstr . ' dbname=postgres';
$node_B->safe_psql('postgres', "CREATE PUBLICATION tap_pub_B FOR TABLE tab");
$node_A->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION $subname_AB
	CONNECTION '$node_B_connstr application_name=$subname_AB'
	PUBLICATION tap_pub_B
	WITH (origin = none, copy_data = off)");

# Wait for initial table sync to finish
$node_A->wait_for_subscription_sync($node_B, $subname_AB);
$node_B->wait_for_subscription_sync($node_A, $subname_BA);

is(1, 1, 'Bidirectional replication setup is complete');

###############################################################################
# Check that bidirectional logical replication setup does not cause infinite
# recursive insertion.
###############################################################################

# insert a record
$node_A->safe_psql('postgres', "INSERT INTO tab VALUES (11);");
$node_B->safe_psql('postgres', "INSERT INTO tab VALUES (21);");

$node_A->wait_for_catchup($subname_BA);
$node_B->wait_for_catchup($subname_AB);

# check that transaction was committed on subscriber(s)
$result = $node_A->safe_psql('postgres', "SELECT * FROM tab ORDER BY 1;");
is( $result, qq(11
21),
	'Inserted successfully without leading to infinite recursion in bidirectional replication setup'
);
$result = $node_B->safe_psql('postgres', "SELECT * FROM tab ORDER BY 1;");
is( $result, qq(11
21),
	'Inserted successfully without leading to infinite recursion in bidirectional replication setup'
);

$node_A->safe_psql('postgres', "DELETE FROM tab;");

$node_A->wait_for_catchup($subname_BA);
$node_B->wait_for_catchup($subname_AB);

###############################################################################
# Check that remote data of node_B (that originated from node_C) is not
# published to node_A.
###############################################################################
$result = $node_A->safe_psql('postgres', "SELECT * FROM tab ORDER BY 1;");
is($result, qq(), 'Check existing data');

$result = $node_B->safe_psql('postgres', "SELECT * FROM tab ORDER BY 1;");
is($result, qq(), 'Check existing data');

# Initialize node node_C
my $node_C = PostgreSQL::Test::Cluster->new('node_C');
$node_C->init(allows_streaming => 'logical');
$node_C->start;

$node_C->safe_psql('postgres', "CREATE TABLE tab (a int PRIMARY KEY)");

# Setup logical replication
# node_C (pub) -> node_B (sub)
my $node_C_connstr = $node_C->connstr . ' dbname=postgres';
$node_C->safe_psql('postgres', "CREATE PUBLICATION tap_pub_C FOR TABLE tab");
$node_B->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION $subname_BC
	CONNECTION '$node_C_connstr application_name=$subname_BC'
	PUBLICATION tap_pub_C
	WITH (origin = none)");
$node_B->wait_for_subscription_sync($node_C, $subname_BC);

# insert a record
$node_C->safe_psql('postgres', "INSERT INTO tab VALUES (32);");

$node_C->wait_for_catchup($subname_BC);
$node_B->wait_for_catchup($subname_AB);
$node_A->wait_for_catchup($subname_BA);

$result = $node_B->safe_psql('postgres', "SELECT * FROM tab ORDER BY 1;");
is($result, qq(32), 'The node_C data replicated to node_B');

# check that the data published from node_C to node_B is not sent to node_A
$result = $node_A->safe_psql('postgres', "SELECT * FROM tab ORDER BY 1;");
is($result, qq(),
	'Remote data originating from another node (not the publisher) is not replicated when origin parameter is none'
);

###############################################################################
# Check that the conflict can be detected when attempting to update or
# delete a row that was previously modified by a different source.
###############################################################################

$node_B->safe_psql('postgres', "DELETE FROM tab;");

$node_A->safe_psql('postgres', "INSERT INTO tab VALUES (32);");

$node_A->wait_for_catchup($subname_BA);
$node_B->wait_for_catchup($subname_AB);

$result = $node_B->safe_psql('postgres', "SELECT * FROM tab ORDER BY 1;");
is($result, qq(32), 'The node_A data replicated to node_B');

# The update should update the row on node B that was inserted by node A.
$node_C->safe_psql('postgres', "UPDATE tab SET a = 33 WHERE a = 32;");

$node_B->wait_for_log(
	qr/conflict detected on relation "public.tab": conflict=update_origin_differs.*\n.*DETAIL:.* Updating the row that was modified by a different origin ".*" in transaction [0-9]+ at .*\n.*Existing local tuple \(32\); remote tuple \(33\); replica identity \(a\)=\(32\)/
);

$node_B->safe_psql('postgres', "DELETE FROM tab;");
$node_A->safe_psql('postgres', "INSERT INTO tab VALUES (33);");

$node_A->wait_for_catchup($subname_BA);
$node_B->wait_for_catchup($subname_AB);

$result = $node_B->safe_psql('postgres', "SELECT * FROM tab ORDER BY 1;");
is($result, qq(33), 'The node_A data replicated to node_B');

# The delete should remove the row on node B that was inserted by node A.
$node_C->safe_psql('postgres', "DELETE FROM tab WHERE a = 33;");

$node_B->wait_for_log(
	qr/conflict detected on relation "public.tab": conflict=delete_origin_differs.*\n.*DETAIL:.* Deleting the row that was modified by a different origin ".*" in transaction [0-9]+ at .*\n.*Existing local tuple \(33\); replica identity \(a\)=\(33\)/
);

# The remaining tests no longer test conflict detection.
$node_B->append_conf('postgresql.conf', 'track_commit_timestamp = off');
$node_B->restart;

###############################################################################
# Specifying origin = NONE indicates that the publisher should only replicate the
# changes that are generated locally from node_B, but in this case since the
# node_B is also subscribing data from node_A, node_B can have remotely
# originated data from node_A. We log a warning, in this case, to draw
# attention to there being possible remote data.
###############################################################################
($result, $stdout, $stderr) = $node_A->psql(
	'postgres', "
        CREATE SUBSCRIPTION $subname_AB2
        CONNECTION '$node_B_connstr application_name=$subname_AB2'
        PUBLICATION tap_pub_B
        WITH (origin = none, copy_data = on)");
like(
	$stderr,
	qr/WARNING: ( [A-Z0-9]+:)? subscription "tap_sub_a_b_2" requested copy_data with origin = NONE but might copy data that had a different origin/,
	"Create subscription with origin = none and copy_data when the publisher has subscribed same table"
);

$node_A->wait_for_subscription_sync($node_B, $subname_AB2);

# Alter subscription ... refresh publication should be successful when no new
# table is added
$node_A->safe_psql(
	'postgres', "
        ALTER SUBSCRIPTION $subname_AB2 REFRESH PUBLICATION");

# Check Alter subscription ... refresh publication when there is a new
# table that is subscribing data from a different publication
$node_A->safe_psql('postgres', "CREATE TABLE tab_new (a int PRIMARY KEY)");
$node_B->safe_psql('postgres', "CREATE TABLE tab_new (a int PRIMARY KEY)");

# add a new table to the publication
$node_A->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_A ADD TABLE tab_new");
$node_B->safe_psql(
	'postgres', "
        ALTER SUBSCRIPTION $subname_BA REFRESH PUBLICATION");

$node_B->wait_for_subscription_sync($node_A, $subname_BA);

# add a new table to the publication
$node_B->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_B ADD TABLE tab_new");

# Alter subscription ... refresh publication should log a warning when a new
# table on the publisher is subscribing data from a different publication
($result, $stdout, $stderr) = $node_A->psql(
	'postgres', "
        ALTER SUBSCRIPTION $subname_AB2 REFRESH PUBLICATION");
like(
	$stderr,
	qr/WARNING: ( [A-Z0-9]+:)? subscription "tap_sub_a_b_2" requested copy_data with origin = NONE but might copy data that had a different origin/,
	"Refresh publication when the publisher has subscribed for the new table, but the subscriber-side wants origin = none"
);

# Ensure that relation has reached 'ready' state before we try to drop it
my $synced_query =
  "SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('r');";
$node_A->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

$node_B->wait_for_catchup($subname_AB2);

# clear the operations done by this test
$node_A->safe_psql('postgres', "DROP TABLE tab_new");
$node_B->safe_psql('postgres', "DROP TABLE tab_new");
$node_A->safe_psql('postgres', "DROP SUBSCRIPTION $subname_AB2");

# shutdown
$node_B->stop('fast');
$node_A->stop('fast');
$node_C->stop('fast');

done_testing();
