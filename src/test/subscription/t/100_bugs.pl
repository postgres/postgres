# Tests for various bugs found over time
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 7;

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

# https://postgr.es/m/OS0PR01MB61133CA11630DAE45BC6AD95FB939%40OS0PR01MB6113.jpnprd01.prod.outlook.com

# The bug was that when changing the REPLICA IDENTITY INDEX to another one, the
# target table's relcache was not being invalidated. This leads to skipping
# UPDATE/DELETE operations during apply on the subscriber side as the columns
# required to search corresponding rows won't get logged.
$node_publisher = get_new_node('publisher3');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

$node_subscriber = get_new_node('subscriber3');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_replidentity_index(a int not null, b int not null)");
$node_publisher->safe_psql('postgres',
	"CREATE UNIQUE INDEX idx_replidentity_index_a ON tab_replidentity_index(a)"
);
$node_publisher->safe_psql('postgres',
	"CREATE UNIQUE INDEX idx_replidentity_index_b ON tab_replidentity_index(b)"
);

# use index idx_replidentity_index_a as REPLICA IDENTITY on publisher.
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_replidentity_index REPLICA IDENTITY USING INDEX idx_replidentity_index_a"
);

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_replidentity_index VALUES(1, 1),(2, 2)");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_replidentity_index(a int not null, b int not null)");
$node_subscriber->safe_psql('postgres',
	"CREATE UNIQUE INDEX idx_replidentity_index_a ON tab_replidentity_index(a)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE UNIQUE INDEX idx_replidentity_index_b ON tab_replidentity_index(b)"
);
# use index idx_replidentity_index_b as REPLICA IDENTITY on subscriber because
# it reflects the future scenario we are testing: changing REPLICA IDENTITY
# INDEX.
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_replidentity_index REPLICA IDENTITY USING INDEX idx_replidentity_index_b"
);

$publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE tab_replidentity_index");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT * FROM tab_replidentity_index"),
	qq(1|1
2|2),
	"check initial data on subscriber");

# Set REPLICA IDENTITY to idx_replidentity_index_b on publisher, then run UPDATE and DELETE.
$node_publisher->safe_psql(
	'postgres', qq[
	ALTER TABLE tab_replidentity_index REPLICA IDENTITY USING INDEX idx_replidentity_index_b;
	UPDATE tab_replidentity_index SET a = -a WHERE a = 1;
	DELETE FROM tab_replidentity_index WHERE a = 2;
]);

$node_publisher->wait_for_catchup('tap_sub');
is( $node_subscriber->safe_psql(
		'postgres', "SELECT * FROM tab_replidentity_index"),
	qq(-1|1),
	"update works with REPLICA IDENTITY");

$node_publisher->stop('fast');
$node_subscriber->stop('fast');

# The bug was that when the REPLICA IDENTITY FULL is used with dropped or
# generated columns, we fail to apply updates and deletes
my $node_publisher_d_cols = get_new_node('node_publisher_d_cols');
$node_publisher_d_cols->init(allows_streaming => 'logical');
$node_publisher_d_cols->start;

my $node_subscriber_d_cols = get_new_node('node_subscriber_d_cols');
$node_subscriber_d_cols->init(allows_streaming => 'logical');
$node_subscriber_d_cols->start;

$node_publisher_d_cols->safe_psql(
	'postgres', qq(
	CREATE TABLE dropped_cols (a int, b_drop int, c int);
	ALTER TABLE dropped_cols REPLICA IDENTITY FULL;
	CREATE TABLE generated_cols (a int, b_gen int GENERATED ALWAYS AS (5 * a) STORED, c int);
	ALTER TABLE generated_cols REPLICA IDENTITY FULL;
	CREATE PUBLICATION pub_dropped_cols FOR TABLE dropped_cols, generated_cols;
	-- some initial data
	INSERT INTO dropped_cols VALUES (1, 1, 1);
	INSERT INTO generated_cols (a, c) VALUES (1, 1);
));

$node_subscriber_d_cols->safe_psql(
	'postgres', qq(
	 CREATE TABLE dropped_cols (a int, b_drop int, c int);
	 CREATE TABLE generated_cols (a int, b_gen int GENERATED ALWAYS AS (5 * a) STORED, c int);
));

my $publisher_connstr_d_cols =
  $node_publisher_d_cols->connstr . ' dbname=postgres';
$node_subscriber_d_cols->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub_dropped_cols CONNECTION '$publisher_connstr_d_cols application_name=sub_dropped_cols' PUBLICATION pub_dropped_cols"
);
$node_subscriber_d_cols->wait_for_subscription_sync($node_publisher_d_cols,
	'sub_dropped_cols');

$node_publisher_d_cols->safe_psql(
	'postgres', qq(
		ALTER TABLE dropped_cols DROP COLUMN b_drop;
));
$node_subscriber_d_cols->safe_psql(
	'postgres', qq(
		ALTER TABLE dropped_cols DROP COLUMN b_drop;
));

$node_publisher_d_cols->safe_psql(
	'postgres', qq(
		UPDATE dropped_cols SET a = 100;
		UPDATE generated_cols SET a = 100;
));
$node_publisher_d_cols->wait_for_catchup('sub_dropped_cols');

is( $node_subscriber_d_cols->safe_psql(
		'postgres', "SELECT count(*) FROM dropped_cols WHERE a = 100"),
	qq(1),
	'replication with RI FULL and dropped columns');

is( $node_subscriber_d_cols->safe_psql(
		'postgres', "SELECT count(*) FROM generated_cols WHERE a = 100"),
	qq(1),
	'replication with RI FULL and generated columns');

$node_publisher_d_cols->stop('fast');
$node_subscriber_d_cols->stop('fast');
