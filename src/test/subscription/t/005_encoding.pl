# Test replication between databases with different encodings
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 1;

my $node_publisher = get_new_node('publisher');
$node_publisher->init(
	allows_streaming => 'logical',
	extra            => [ '--locale=C', '--encoding=UTF8' ]);
$node_publisher->start;

my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(
	allows_streaming => 'logical',
	extra            => [ '--locale=C', '--encoding=LATIN1' ]);
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

# Wait for initial sync to finish as well
my $synced_query =
  "SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('s', 'r');";
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

$node_publisher->safe_psql('postgres',
	q{INSERT INTO test1 VALUES (1, E'Mot\xc3\xb6rhead')}); # hand-rolled UTF-8

$node_publisher->wait_for_catchup('mysub');

is( $node_subscriber->safe_psql(
		'postgres', q{SELECT a FROM test1 WHERE b = E'Mot\xf6rhead'}
	),                                                     # LATIN1
	qq(1),
	'data replicated to subscriber');

$node_subscriber->stop;
$node_publisher->stop;
