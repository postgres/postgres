
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test collations, in particular nondeterministic ones
# (only works with ICU)
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{with_icu} ne 'yes')
{
	plan skip_all => 'ICU not supported by this build';
}

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(
	allows_streaming => 'logical',
	extra            => [ '--locale=C', '--encoding=UTF8' ]);
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(
	allows_streaming => 'logical',
	extra            => [ '--locale=C', '--encoding=UTF8' ]);
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

# Test plan: Create a table with a nondeterministic collation in the
# primary key column.  Pre-insert rows on the publisher and subscriber
# that are collation-wise equal but byte-wise different.  (We use a
# string in different normal forms for that.)  Set up publisher and
# subscriber.  Update the row on the publisher, but don't change the
# primary key column.  The subscriber needs to find the row to be
# updated using the nondeterministic collation semantics.  We need to
# test for both a replica identity index and for replica identity
# full, since those have different code paths internally.

$node_subscriber->safe_psql('postgres',
	q{CREATE COLLATION ctest_nondet (provider = icu, locale = 'und', deterministic = false)}
);

# table with replica identity index

$node_publisher->safe_psql('postgres',
	q{CREATE TABLE tab1 (a text PRIMARY KEY, b text)});

$node_publisher->safe_psql('postgres',
	q{INSERT INTO tab1 VALUES (U&'\00E4bc', 'foo')});

$node_subscriber->safe_psql('postgres',
	q{CREATE TABLE tab1 (a text COLLATE ctest_nondet PRIMARY KEY, b text)});

$node_subscriber->safe_psql('postgres',
	q{INSERT INTO tab1 VALUES (U&'\0061\0308bc', 'foo')});

# table with replica identity full

$node_publisher->safe_psql('postgres', q{CREATE TABLE tab2 (a text, b text)});
$node_publisher->safe_psql('postgres',
	q{ALTER TABLE tab2 REPLICA IDENTITY FULL});

$node_publisher->safe_psql('postgres',
	q{INSERT INTO tab2 VALUES (U&'\00E4bc', 'foo')});

$node_subscriber->safe_psql('postgres',
	q{CREATE TABLE tab2 (a text COLLATE ctest_nondet, b text)});
$node_subscriber->safe_psql('postgres',
	q{ALTER TABLE tab2 REPLICA IDENTITY FULL});

$node_subscriber->safe_psql('postgres',
	q{INSERT INTO tab2 VALUES (U&'\0061\0308bc', 'foo')});

# set up publication, subscription

$node_publisher->safe_psql('postgres',
	q{CREATE PUBLICATION pub1 FOR ALL TABLES});

$node_subscriber->safe_psql('postgres',
	qq{CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1 WITH (copy_data = false)}
);

$node_publisher->wait_for_catchup('sub1');

# test with replica identity index

$node_publisher->safe_psql('postgres',
	q{UPDATE tab1 SET b = 'bar' WHERE b = 'foo'});

$node_publisher->wait_for_catchup('sub1');

is($node_subscriber->safe_psql('postgres', q{SELECT b FROM tab1}),
	qq(bar), 'update with primary key with nondeterministic collation');

# test with replica identity full

$node_publisher->safe_psql('postgres',
	q{UPDATE tab2 SET b = 'bar' WHERE b = 'foo'});

$node_publisher->wait_for_catchup('sub1');

is($node_subscriber->safe_psql('postgres', q{SELECT b FROM tab2}),
	qq(bar),
	'update with replica identity full with nondeterministic collation');

done_testing();
