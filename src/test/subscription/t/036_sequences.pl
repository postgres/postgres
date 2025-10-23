
# Copyright (c) 2025, PostgreSQL Global Development Group

# This tests that sequences are registered to be synced to the subscriber
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');

# Avoid checkpoint during the test, otherwise, extra values will be fetched for
# the sequences which will cause the test to fail randomly.
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Initialize subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Setup structure on the publisher
my $ddl = qq(
	CREATE TABLE regress_seq_test (v BIGINT);
	CREATE SEQUENCE regress_s1;
);
$node_publisher->safe_psql('postgres', $ddl);

# Setup the same structure on the subscriber
$node_subscriber->safe_psql('postgres', $ddl);

# Insert initial test data
$node_publisher->safe_psql(
	'postgres', qq(
	-- generate a number of values using the sequence
	INSERT INTO regress_seq_test SELECT nextval('regress_s1') FROM generate_series(1,100);
));

# Setup logical replication pub/sub
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION regress_seq_pub FOR ALL SEQUENCES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION regress_seq_sub CONNECTION '$publisher_connstr' PUBLICATION regress_seq_pub"
);

# Confirm sequences can be listed in pg_subscription_rel
my $result = $node_subscriber->safe_psql('postgres',
	"SELECT relname, srsubstate FROM pg_class, pg_subscription_rel WHERE oid = srrelid"
);
is($result, 'regress_s1|i', "Sequence can be in pg_subscription_rel catalog");

done_testing();
