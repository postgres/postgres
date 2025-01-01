
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

# Logical replication tests for temporal tables
#
# A table can use a temporal PRIMARY KEY or UNIQUE index as its REPLICA IDENTITY.
# This is a GiST index (not B-tree) and its last element uses WITHOUT OVERLAPS.
# That element restricts other rows with overlaps semantics instead of equality,
# but it is always at least as restrictive as a normal non-null unique index.
# Therefore we can still apply logical decoding messages to the subscriber.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# setup

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

my ($result, $stdout, $stderr);

sub create_tables()
{
	# create tables on publisher

	$node_publisher->safe_psql('postgres',
		"CREATE TABLE temporal_no_key (id int4range, valid_at daterange, a text)"
	);

	$node_publisher->safe_psql('postgres',
		"CREATE TABLE temporal_pk (id int4range, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
	);

	$node_publisher->safe_psql('postgres',
		"CREATE TABLE temporal_unique (id int4range, valid_at daterange, a text, UNIQUE (id, valid_at WITHOUT OVERLAPS))"
	);

	# create tables on subscriber

	$node_subscriber->safe_psql('postgres',
		"CREATE TABLE temporal_no_key (id int4range, valid_at daterange, a text)"
	);

	$node_subscriber->safe_psql('postgres',
		"CREATE TABLE temporal_pk (id int4range, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
	);

	$node_subscriber->safe_psql('postgres',
		"CREATE TABLE temporal_unique (id int4range, valid_at daterange, a text, UNIQUE (id, valid_at WITHOUT OVERLAPS))"
	);
}

sub drop_everything()
{
	$node_publisher->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_no_key");
	$node_publisher->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_pk");
	$node_publisher->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_unique");
	$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub1");
	$node_subscriber->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_no_key");
	$node_subscriber->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_pk");
	$node_subscriber->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_unique");
	$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION sub1");
}

# #################################
# Test with REPLICA IDENTITY DEFAULT:
# #################################

create_tables();

# sync initial data:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);
$node_subscriber->wait_for_subscription_sync;

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_no_key DEFAULT');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is($result, qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_pk DEFAULT');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_unique DEFAULT');

# replicate with no key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"UPDATE temporal_no_key SET a = 'b' WHERE id = '[2,3)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot update table "temporal_no_key" because it does not have a replica identity and publishes updates
HINT:  To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't UPDATE temporal_no_key DEFAULT");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_no_key WHERE id = '[3,4)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_no_key" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_no_key DEFAULT");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|a
[3,4)|[2000-01-01,2010-01-01)|a
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_no_key DEFAULT');

# replicate with a primary key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_pk SET a = 'b' WHERE id = '[2,3)'");

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_pk WHERE id = '[3,4)'");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_pk DEFAULT');

# replicate with a unique key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"UPDATE temporal_unique SET a = 'b' WHERE id = '[2,3)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot update table "temporal_unique" because it does not have a replica identity and publishes updates
HINT:  To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't UPDATE temporal_unique DEFAULT");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_unique WHERE id = '[3,4)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_unique" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_unique DEFAULT");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|a
[3,4)|[2000-01-01,2010-01-01)|a
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_unique DEFAULT');

# cleanup

drop_everything();


# #################################
# Test with REPLICA IDENTITY FULL:
# #################################

create_tables();

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_no_key REPLICA IDENTITY FULL");

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY FULL");

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY FULL");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_no_key REPLICA IDENTITY FULL");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY FULL");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY FULL");

# sync initial data:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);
$node_subscriber->wait_for_subscription_sync;

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_no_key FULL');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is($result, qq{[1,2)|[2000-01-01,2010-01-01)|a}, 'synced temporal_pk FULL');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_unique FULL');

# replicate with no key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_no_key SET a = 'b' WHERE id = '[2,3)'");

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_no_key WHERE id = '[3,4)'");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_no_key FULL');

# replicate with a primary key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_pk SET a = 'b' WHERE id = '[2,3)'");

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_pk WHERE id = '[3,4)'");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_pk FULL');

# replicate with a unique key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_unique SET a = 'b' WHERE id = '[2,3)'");

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_unique WHERE id = '[3,4)'");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_unique FULL');

# cleanup

drop_everything();


# #################################
# Test with REPLICA IDENTITY USING INDEX
# #################################

# create tables on publisher

$node_publisher->safe_psql('postgres',
	"CREATE TABLE temporal_pk (id int4range, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
);
$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY USING INDEX temporal_pk_pkey");

$node_publisher->safe_psql('postgres',
	"CREATE TABLE temporal_unique (id int4range NOT NULL, valid_at daterange NOT NULL, a text, UNIQUE (id, valid_at WITHOUT OVERLAPS))"
);
$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY USING INDEX temporal_unique_id_valid_at_key"
);

# create tables on subscriber

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE temporal_pk (id int4range, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
);
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY USING INDEX temporal_pk_pkey");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE temporal_unique (id int4range NOT NULL, valid_at daterange NOT NULL, a text, UNIQUE (id, valid_at WITHOUT OVERLAPS))"
);
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY USING INDEX temporal_unique_id_valid_at_key"
);

# sync initial data:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);
$node_subscriber->wait_for_subscription_sync;

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_pk USING INDEX');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_unique USING INDEX');

# replicate with a primary key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_pk SET a = 'b' WHERE id = '[2,3)'");

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_pk WHERE id = '[3,4)'");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_pk USING INDEX');

# replicate with a unique key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_unique SET a = 'b' WHERE id = '[2,3)'");

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_unique WHERE id = '[3,4)'");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_unique USING INDEX');

# cleanup

drop_everything();


# #################################
# Test with REPLICA IDENTITY NOTHING
# #################################

create_tables();

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_no_key REPLICA IDENTITY NOTHING");

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY NOTHING");

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY NOTHING");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_no_key REPLICA IDENTITY NOTHING");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY NOTHING");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY NOTHING");

# sync initial data:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);
$node_subscriber->wait_for_subscription_sync;

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_no_key NOTHING');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is($result, qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_pk NOTHING');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_unique NOTHING');

# replicate with no key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"UPDATE temporal_no_key SET a = 'b' WHERE id = '[2,3)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot update table "temporal_no_key" because it does not have a replica identity and publishes updates
HINT:  To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't UPDATE temporal_no_key NOTHING");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_no_key WHERE id = '[3,4)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_no_key" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_no_key NOTHING");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|a
[3,4)|[2000-01-01,2010-01-01)|a
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_no_key NOTHING');

# replicate with a primary key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"UPDATE temporal_pk SET a = 'b' WHERE id = '[2,3)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot update table "temporal_pk" because it does not have a replica identity and publishes updates
HINT:  To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't UPDATE temporal_pk NOTHING");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_pk WHERE id = '[3,4)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_pk" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_pk NOTHING");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|a
[3,4)|[2000-01-01,2010-01-01)|a
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_pk NOTHING');

# replicate with a unique key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"UPDATE temporal_unique SET a = 'b' WHERE id = '[2,3)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot update table "temporal_unique" because it does not have a replica identity and publishes updates
HINT:  To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't UPDATE temporal_unique NOTHING");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_unique WHERE id = '[3,4)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_unique" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_unique NOTHING");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|a
[3,4)|[2000-01-01,2010-01-01)|a
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_unique NOTHING');

# cleanup

drop_everything();

done_testing();
