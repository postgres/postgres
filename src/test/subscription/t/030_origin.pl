
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test the CREATE SUBSCRIPTION 'origin' parameter.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

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
$node_B->start;

# Create table on node_A
$node_A->safe_psql('postgres', "CREATE TABLE tab (a int PRIMARY KEY)");

# Create the same table on node_B
$node_B->safe_psql('postgres', "CREATE TABLE tab (a int PRIMARY KEY)");

# Setup logical replication
# node_A (pub) -> node_B (sub)
my $node_A_connstr = $node_A->connstr . ' dbname=postgres';
$node_A->safe_psql('postgres', "CREATE PUBLICATION tap_pub_A FOR TABLE tab");
my $appname_B1 = 'tap_sub_B1';
$node_B->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION tap_sub_B1
	CONNECTION '$node_A_connstr application_name=$appname_B1'
	PUBLICATION tap_pub_A
	WITH (origin = none)");

# node_B (pub) -> node_A (sub)
my $node_B_connstr = $node_B->connstr . ' dbname=postgres';
$node_B->safe_psql('postgres', "CREATE PUBLICATION tap_pub_B FOR TABLE tab");
my $appname_A = 'tap_sub_A';
$node_A->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION tap_sub_A
	CONNECTION '$node_B_connstr application_name=$appname_A'
	PUBLICATION tap_pub_B
	WITH (origin = none, copy_data = off)");

# Wait for initial table sync to finish
$node_A->wait_for_subscription_sync($node_B, $appname_A);
$node_B->wait_for_subscription_sync($node_A, $appname_B1);

is(1, 1, 'Bidirectional replication setup is complete');

my $result;

###############################################################################
# Check that bidirectional logical replication setup does not cause infinite
# recursive insertion.
###############################################################################

# insert a record
$node_A->safe_psql('postgres', "INSERT INTO tab VALUES (11);");
$node_B->safe_psql('postgres', "INSERT INTO tab VALUES (21);");

$node_A->wait_for_catchup($appname_B1);
$node_B->wait_for_catchup($appname_A);

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

$node_A->wait_for_catchup($appname_B1);
$node_B->wait_for_catchup($appname_A);

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

my $appname_B2 = 'tap_sub_B2';
$node_B->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION tap_sub_B2
	CONNECTION '$node_C_connstr application_name=$appname_B2'
	PUBLICATION tap_pub_C
	WITH (origin = none)");

$node_B->wait_for_subscription_sync($node_C, $appname_B2);

# insert a record
$node_C->safe_psql('postgres', "INSERT INTO tab VALUES (32);");

$node_C->wait_for_catchup($appname_B2);
$node_B->wait_for_catchup($appname_A);
$node_A->wait_for_catchup($appname_B1);

$result = $node_B->safe_psql('postgres', "SELECT * FROM tab ORDER BY 1;");
is($result, qq(32), 'The node_C data replicated to node_B');

# check that the data published from node_C to node_B is not sent to node_A
$result = $node_A->safe_psql('postgres', "SELECT * FROM tab ORDER BY 1;");
is($result, qq(),
	'Remote data originating from another node (not the publisher) is not replicated when origin parameter is none'
);

# shutdown
$node_B->stop('fast');
$node_A->stop('fast');
$node_C->stop('fast');

done_testing();
