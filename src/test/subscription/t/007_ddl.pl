
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test some logical replication DDL behavior
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

my $ddl = "CREATE TABLE test1 (a int, b text);";
$node_publisher->safe_psql('postgres', $ddl);
$node_subscriber->safe_psql('postgres', $ddl);

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION mypub FOR ALL TABLES;");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION mysub CONNECTION '$publisher_connstr' PUBLICATION mypub;"
);

$node_publisher->wait_for_catchup('mysub');

$node_subscriber->safe_psql(
	'postgres', q{
BEGIN;
ALTER SUBSCRIPTION mysub DISABLE;
ALTER SUBSCRIPTION mysub SET (slot_name = NONE);
DROP SUBSCRIPTION mysub;
COMMIT;
});

pass "subscription disable and drop in same transaction did not hang";

# One of the specified publications exists.
my ($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION mysub1 CONNECTION '$publisher_connstr' PUBLICATION mypub, non_existent_pub"
);
ok( $stderr =~
	  m/WARNING:  publication "non_existent_pub" does not exist on the publisher/,
	"Create subscription throws warning for non-existent publication");

# Wait for initial table sync to finish.
$node_subscriber->wait_for_subscription_sync($node_publisher, 'mysub1');

# Specifying non-existent publication along with add publication.
($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"ALTER SUBSCRIPTION mysub1 ADD PUBLICATION non_existent_pub1, non_existent_pub2"
);
ok( $stderr =~
	  m/WARNING:  publications "non_existent_pub1", "non_existent_pub2" do not exist on the publisher/,
	"Alter subscription add publication throws warning for non-existent publications"
);

# Specifying non-existent publication along with set publication.
($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"ALTER SUBSCRIPTION mysub1 SET PUBLICATION non_existent_pub");
ok( $stderr =~
	  m/WARNING:  publication "non_existent_pub" does not exist on the publisher/,
	"Alter subscription set publication throws warning for non-existent publication"
);

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
