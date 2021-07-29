
# Copyright (c) 2021, PostgreSQL Global Development Group

# Binary mode logical replication test

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 5;

# Create and initialize a publisher node
my $node_publisher = PostgresNode->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create and initialize subscriber node
my $node_subscriber = PostgresNode->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Create tables on both sides of the replication
my $ddl = qq(
	CREATE TABLE public.test_numerical (
		a INTEGER PRIMARY KEY,
		b NUMERIC,
		c FLOAT,
		d BIGINT
		);
	CREATE TABLE public.test_arrays (
		a INTEGER[] PRIMARY KEY,
		b NUMERIC[],
		c TEXT[]
		););

$node_publisher->safe_psql('postgres', $ddl);
$node_subscriber->safe_psql('postgres', $ddl);

# Configure logical replication
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tpub FOR ALL TABLES");

my $publisher_connstring = $node_publisher->connstr . ' dbname=postgres';
$node_subscriber->safe_psql('postgres',
	    "CREATE SUBSCRIPTION tsub CONNECTION '$publisher_connstring' "
	  . "PUBLICATION tpub WITH (slot_name = tpub_slot, binary = true)");

# Ensure nodes are in sync with each other
$node_publisher->wait_for_catchup('tsub');
$node_subscriber->poll_query_until('postgres',
	"SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('s', 'r');"
) or die "Timed out while waiting for subscriber to synchronize data";

# Insert some content and make sure it's replicated across
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO public.test_arrays (a, b, c) VALUES
		('{1,2,3}', '{1.1, 1.2, 1.3}', '{"one", "two", "three"}'),
		('{3,1,2}', '{1.3, 1.1, 1.2}', '{"three", "one", "two"}');

	INSERT INTO public.test_numerical (a, b, c, d) VALUES
		(1, 1.2, 1.3, 10),
		(2, 2.2, 2.3, 20),
		(3, 3.2, 3.3, 30);
	));

$node_publisher->wait_for_catchup('tsub');

my $result = $node_subscriber->safe_psql('postgres',
	"SELECT a, b, c, d FROM test_numerical ORDER BY a");

is( $result, '1|1.2|1.3|10
2|2.2|2.3|20
3|3.2|3.3|30', 'check replicated data on subscriber');

# Test updates as well
$node_publisher->safe_psql(
	'postgres', qq(
	UPDATE public.test_arrays SET b[1] = 42, c = NULL;
	UPDATE public.test_numerical SET b = 42, c = NULL;
	));

$node_publisher->wait_for_catchup('tsub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT a, b, c FROM test_arrays ORDER BY a");

is( $result, '{1,2,3}|{42,1.2,1.3}|
{3,1,2}|{42,1.1,1.2}|', 'check updated replicated data on subscriber');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT a, b, c, d FROM test_numerical ORDER BY a");

is( $result, '1|42||10
2|42||20
3|42||30', 'check updated replicated data on subscriber');

# Test to reset back to text formatting, and then to binary again
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tsub SET (binary = false);");

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO public.test_numerical (a, b, c, d) VALUES
		(4, 4.2, 4.3, 40);
	));

$node_publisher->wait_for_catchup('tsub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT a, b, c, d FROM test_numerical ORDER BY a");

is( $result, '1|42||10
2|42||20
3|42||30
4|4.2|4.3|40', 'check replicated data on subscriber');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tsub SET (binary = true);");

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO public.test_arrays (a, b, c) VALUES
		('{2,3,1}', '{1.2, 1.3, 1.1}', '{"two", "three", "one"}');
	));

$node_publisher->wait_for_catchup('tsub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT a, b, c FROM test_arrays ORDER BY a");

is( $result, '{1,2,3}|{42,1.2,1.3}|
{2,3,1}|{1.2,1.3,1.1}|{two,three,one}
{3,1,2}|{42,1.1,1.2}|', 'check replicated data on subscriber');

$node_subscriber->stop('fast');
$node_publisher->stop('fast');
