
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Binary mode logical replication test

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Create and initialize a publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create and initialize subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
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

# ------------------------------------------------------
# Ensure binary mode also executes COPY in binary format
# ------------------------------------------------------

# Insert some content before creating a subscription
$node_publisher->safe_psql(
	'postgres', qq(
    INSERT INTO public.test_numerical (a, b, c, d) VALUES
		(1, 1.2, 1.3, 10),
        (2, 2.2, 2.3, 20);
	INSERT INTO public.test_arrays (a, b, c) VALUES
		('{1,2,3}', '{1.1, 1.2, 1.3}', '{"one", "two", "three"}'),
        ('{3,1,2}', '{1.3, 1.1, 1.2}', '{"three", "one", "two"}');
	));

my $publisher_connstring = $node_publisher->connstr . ' dbname=postgres';
$node_subscriber->safe_psql('postgres',
		"CREATE SUBSCRIPTION tsub CONNECTION '$publisher_connstring' "
	  . "PUBLICATION tpub WITH (slot_name = tpub_slot, binary = true)");

# Ensure the COPY command is executed in binary format on the publisher
$node_publisher->wait_for_log(
	qr/LOG: ( [A-Z0-9]+:)? statement: COPY (.+)? TO STDOUT WITH \(FORMAT binary\)/
);

# Ensure nodes are in sync with each other
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tsub');

my $sync_check = qq(
	SELECT a, b, c, d FROM test_numerical ORDER BY a;
	SELECT a, b, c FROM test_arrays ORDER BY a;
);

# Check the synced data on the subscriber
my $result = $node_subscriber->safe_psql('postgres', $sync_check);

is( $result, '1|1.2|1.3|10
2|2.2|2.3|20
{1,2,3}|{1.1,1.2,1.3}|{one,two,three}
{3,1,2}|{1.3,1.1,1.2}|{three,one,two}', 'check synced data on subscriber');

# ----------------------------------
# Ensure apply works in binary mode
# ----------------------------------

# Insert some content and make sure it's replicated across
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO public.test_arrays (a, b, c) VALUES
		('{2,1,3}', '{1.2, 1.1, 1.3}', '{"two", "one", "three"}'),
		('{1,3,2}', '{1.1, 1.3, 1.2}', '{"one", "three", "two"}');

	INSERT INTO public.test_numerical (a, b, c, d) VALUES
		(3, 3.2, 3.3, 30),
		(4, 4.2, 4.3, 40);
	));

$node_publisher->wait_for_catchup('tsub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT a, b, c, d FROM test_numerical ORDER BY a");

is( $result, '1|1.2|1.3|10
2|2.2|2.3|20
3|3.2|3.3|30
4|4.2|4.3|40', 'check replicated data on subscriber');

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
{1,3,2}|{42,1.3,1.2}|
{2,1,3}|{42,1.1,1.3}|
{3,1,2}|{42,1.1,1.2}|', 'check updated replicated data on subscriber');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT a, b, c, d FROM test_numerical ORDER BY a");

is( $result, '1|42||10
2|42||20
3|42||30
4|42||40', 'check updated replicated data on subscriber');

# ------------------------------------------------------------------------------
# Use ALTER SUBSCRIPTION to change to text format and then back to binary format
# ------------------------------------------------------------------------------

# Test to reset back to text formatting, and then to binary again
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tsub SET (binary = false);");

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO public.test_numerical (a, b, c, d) VALUES
		(5, 5.2, 5.3, 50);
	));

$node_publisher->wait_for_catchup('tsub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT a, b, c, d FROM test_numerical ORDER BY a");

is( $result, '1|42||10
2|42||20
3|42||30
4|42||40
5|5.2|5.3|50', 'check replicated data on subscriber');

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
{1,3,2}|{42,1.3,1.2}|
{2,1,3}|{42,1.1,1.3}|
{2,3,1}|{1.2,1.3,1.1}|{two,three,one}
{3,1,2}|{42,1.1,1.2}|', 'check replicated data on subscriber');

# ---------------------------------------------------------------
# Test binary replication without and with send/receive functions
# ---------------------------------------------------------------

# Create a custom type without send/rcv functions
$ddl = qq(
    CREATE TYPE myvarchar;
    CREATE FUNCTION myvarcharin(cstring, oid, integer) RETURNS myvarchar
        LANGUAGE internal IMMUTABLE PARALLEL SAFE STRICT AS 'varcharin';
    CREATE FUNCTION myvarcharout(myvarchar) RETURNS cstring
        LANGUAGE internal IMMUTABLE PARALLEL SAFE STRICT AS 'varcharout';
    CREATE TYPE myvarchar (
        input = myvarcharin,
        output = myvarcharout);
    CREATE TABLE public.test_myvarchar (
        a myvarchar
    ););

$node_publisher->safe_psql('postgres', $ddl);
$node_subscriber->safe_psql('postgres', $ddl);

# Insert some initial data
$node_publisher->safe_psql(
	'postgres', qq(
    INSERT INTO public.test_myvarchar (a) VALUES
		('a');
    ));

# Check the subscriber log from now on.
my $offset = -s $node_subscriber->logfile;

# Refresh the publication to trigger the tablesync
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tsub REFRESH PUBLICATION");

# It should fail
$node_subscriber->wait_for_log(
	qr/ERROR: ( [A-Z0-9]+:)? no binary input function available for type/,
	$offset);

# Create and set send/rcv functions for the custom type
$ddl = qq(
    CREATE FUNCTION myvarcharsend(myvarchar) RETURNS bytea
        LANGUAGE internal STABLE PARALLEL SAFE STRICT AS 'varcharsend';
    CREATE FUNCTION myvarcharrecv(internal, oid, integer) RETURNS myvarchar
        LANGUAGE internal STABLE PARALLEL SAFE STRICT AS 'varcharrecv';
    ALTER TYPE myvarchar SET (
        send = myvarcharsend,
        receive = myvarcharrecv
    ););

$node_publisher->safe_psql('postgres', $ddl);
$node_subscriber->safe_psql('postgres', $ddl);

# Now tablesync should succeed
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tsub');

# Check the synced data on the subscriber
$result =
  $node_subscriber->safe_psql('postgres', 'SELECT a FROM test_myvarchar;');

is($result, 'a', 'check synced data on subscriber with custom type');

# -----------------------------------------------------
# Test mismatched column types with/without binary mode
# -----------------------------------------------------

# Test syncing tables with mismatching column types
$node_publisher->safe_psql(
	'postgres', qq(
    CREATE TABLE public.test_mismatching_types (
        a bigint PRIMARY KEY
    );
    INSERT INTO public.test_mismatching_types (a)
        VALUES (1), (2);
    ));

# Check the subscriber log from now on.
$offset = -s $node_subscriber->logfile;

$node_subscriber->safe_psql(
	'postgres', qq(
    CREATE TABLE public.test_mismatching_types (
        a int PRIMARY KEY
    );
    ALTER SUBSCRIPTION tsub REFRESH PUBLICATION;
    ));

# Cannot sync due to type mismatch
$node_subscriber->wait_for_log(
	qr/ERROR: ( [A-Z0-9]+:)? incorrect binary data format/, $offset);

# Check the publisher log from now on.
$offset = -s $node_publisher->logfile;

# Setting binary to false should allow syncing
$node_subscriber->safe_psql(
	'postgres', qq(
    ALTER SUBSCRIPTION tsub SET (binary = false);));

# Ensure the COPY command is executed in text format on the publisher
$node_publisher->wait_for_log(
	qr/LOG: ( [A-Z0-9]+:)? statement: COPY (.+)? TO STDOUT\n/, $offset);

$node_subscriber->wait_for_subscription_sync($node_publisher, 'tsub');

# Check the synced data on the subscriber
$result = $node_subscriber->safe_psql('postgres',
	'SELECT a FROM test_mismatching_types ORDER BY a;');

is( $result, '1
2', 'check synced data on subscriber with binary = false');

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
