# Test streaming of simple large transaction
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 4;

# Create publisher node
my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf', 'logical_decoding_work_mem = 64kB');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Create some preexisting content on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b varchar)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_tab VALUES (1, 'foo'), (2, 'bar')");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres', "CREATE TABLE test_tab (a int primary key, b text, c timestamptz DEFAULT now(), d bigint DEFAULT 999)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres', "CREATE PUBLICATION tap_pub FOR TABLE test_tab");

my $appname = 'tap_sub';
$node_subscriber->safe_psql('postgres',
"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub WITH (streaming = on)"
);

$node_publisher->wait_for_catchup($appname);

# Also wait for initial table sync to finish
my $synced_query =
"SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('r', 's');";
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

my $result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(2|2|2), 'check initial data was copied to subscriber');

# Interleave a pair of transactions, each exceeding the 64kB limit.
my $in  = '';
my $out = '';

my $timer = IPC::Run::timeout(180);

my $h = $node_publisher->background_psql('postgres', \$in, \$out, $timer,
	on_error_stop => 0);

$in .= q{
BEGIN;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(3, 5000) s(i);
UPDATE test_tab SET b = md5(b) WHERE mod(a,2) = 0;
DELETE FROM test_tab WHERE mod(a,3) = 0;
};
$h->pump_nb;

$node_publisher->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(5001, 9999) s(i);
DELETE FROM test_tab WHERE a > 5000;
COMMIT;
});

$in .= q{
COMMIT;
\q
};
$h->finish;    # errors make the next test fail, so ignore them here

$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(3334|3334|3334), 'check extra columns contain local defaults');

# Test the streaming in binary mode
$node_subscriber->safe_psql('postgres',
"ALTER SUBSCRIPTION tap_sub SET (binary = on)"
);

# Insert, update and delete enough rows to exceed the 64kB limit.
$node_publisher->safe_psql('postgres', q{
BEGIN;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(5001, 10000) s(i);
UPDATE test_tab SET b = md5(b) WHERE mod(a,2) = 0;
DELETE FROM test_tab WHERE mod(a,3) = 0;
COMMIT;
});

$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(6667|6667|6667), 'check extra columns contain local defaults');

# Change the local values of the extra columns on the subscriber,
# update publisher, and check that subscriber retains the expected
# values. This is to ensure that non-streaming transactions behave
# properly after a streaming transaction.
$node_subscriber->safe_psql('postgres', "UPDATE test_tab SET c = 'epoch'::timestamptz + 987654321 * interval '1s'");
$node_publisher->safe_psql('postgres', "UPDATE test_tab SET b = md5(a::text)");

$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), count(extract(epoch from c) = 987654321), count(d = 999) FROM test_tab");
is($result, qq(6667|6667|6667), 'check extra columns contain locally changed data');

$node_subscriber->stop;
$node_publisher->stop;
