# Test replication between databases with different encodings
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 1;

sub wait_for_caught_up
{
	my ($node, $appname) = @_;

	$node->poll_query_until('postgres',
"SELECT pg_current_wal_lsn() <= replay_lsn FROM pg_stat_replication WHERE application_name = '$appname';"
	) or die "Timed out while waiting for subscriber to catch up";
}

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
my $appname           = 'encoding_test';

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION mypub FOR ALL TABLES;");
$node_subscriber->safe_psql('postgres',
"CREATE SUBSCRIPTION mysub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION mypub;"
);

wait_for_caught_up($node_publisher, $appname);

$node_publisher->safe_psql('postgres',
	q{INSERT INTO test1 VALUES (1, E'Mot\xc3\xb6rhead')}); # hand-rolled UTF-8

wait_for_caught_up($node_publisher, $appname);

is( $node_subscriber->safe_psql(
		'postgres', q{SELECT a FROM test1 WHERE b = E'Mot\xf6rhead'}
	),                                                     # LATIN1
	qq(1),
	'data replicated to subscriber');

$node_subscriber->stop;
$node_publisher->stop;
