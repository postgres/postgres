# Tests for various bugs found over time
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 3;

# Bug #15114

# The bug was that determining which columns are part of the replica
# identity index using RelationGetIndexAttrBitmap() would run
# eval_const_expressions() on index expressions and predicates across
# all indexes of the table, which in turn might require a snapshot,
# but there wasn't one set, so it crashes.  There were actually two
# separate bugs, one on the publisher and one on the subscriber.  The
# fix was to avoid the constant expressions simplification in
# RelationGetIndexAttrBitmap(), so it's safe to call in more contexts.

my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, b int)");

$node_publisher->safe_psql('postgres',
	"CREATE FUNCTION double(x int) RETURNS int IMMUTABLE LANGUAGE SQL AS 'select x * 2'"
);

# an index with a predicate that lends itself to constant expressions
# evaluation
$node_publisher->safe_psql('postgres',
	"CREATE INDEX ON tab1 (b) WHERE a > double(1)");

# and the same setup on the subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, b int)");

$node_subscriber->safe_psql('postgres',
	"CREATE FUNCTION double(x int) RETURNS int IMMUTABLE LANGUAGE SQL AS 'select x * 2'"
);

$node_subscriber->safe_psql('postgres',
	"CREATE INDEX ON tab1 (b) WHERE a > double(1)");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);

$node_publisher->wait_for_catchup('sub1');

# This would crash, first on the publisher, and then (if the publisher
# is fixed) on the subscriber.
$node_publisher->safe_psql('postgres', "INSERT INTO tab1 VALUES (1, 2)");

$node_publisher->wait_for_catchup('sub1');

pass('index predicates do not cause crash');

$node_publisher->stop('fast');
$node_subscriber->stop('fast');


# Handling of temporary and unlogged tables with FOR ALL TABLES publications

# If a FOR ALL TABLES publication exists, temporary and unlogged
# tables are ignored for publishing changes.  The bug was that we
# would still check in that case that such a table has a replica
# identity set before accepting updates.  If it did not it would cause
# an error when an update was attempted.

$node_publisher = get_new_node('publisher2');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub FOR ALL TABLES");

is( $node_publisher->psql(
		'postgres',
		"CREATE TEMPORARY TABLE tt1 AS SELECT 1 AS a; UPDATE tt1 SET a = 2;"),
	0,
	'update to temporary table without replica identity with FOR ALL TABLES publication'
);

is( $node_publisher->psql(
		'postgres',
		"CREATE UNLOGGED TABLE tu1 AS SELECT 1 AS a; UPDATE tu1 SET a = 2;"),
	0,
	'update to unlogged table without replica identity with FOR ALL TABLES publication'
);

$node_publisher->stop('fast');
