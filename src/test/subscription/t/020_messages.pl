
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Tests that logical decoding messages
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Create publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf', 'autovacuum = off');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Create some preexisting content on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_test (a int primary key)");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_test (a int primary key)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE tab_test");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub"
);

$node_publisher->wait_for_catchup('tap_sub');

# Ensure a transactional logical decoding message shows up on the slot
$node_subscriber->safe_psql('postgres', "ALTER SUBSCRIPTION tap_sub DISABLE");

# wait for the replication slot to become inactive on the publisher
$node_publisher->poll_query_until(
	'postgres',
	"SELECT COUNT(*) FROM pg_catalog.pg_replication_slots WHERE slot_name = 'tap_sub' AND active='f'",
	1);

$node_publisher->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'pgoutput', 'a transactional message')"
);

my $result = $node_publisher->safe_psql(
	'postgres', qq(
		SELECT get_byte(data, 0)
		FROM pg_logical_slot_peek_binary_changes('tap_sub', NULL, NULL,
			'proto_version', '1',
			'publication_names', 'tap_pub',
			'messages', 'true')
));

# 66 77 67 == B M C == BEGIN MESSAGE COMMIT
is( $result, qq(66
77
67),
	'messages on slot are B M C with message option');

$result = $node_publisher->safe_psql(
	'postgres', qq(
		SELECT get_byte(data, 1), encode(substr(data, 11, 8), 'escape')
		FROM pg_logical_slot_peek_binary_changes('tap_sub', NULL, NULL,
			'proto_version', '1',
			'publication_names', 'tap_pub',
			'messages', 'true')
		OFFSET 1 LIMIT 1
));

is($result, qq(1|pgoutput),
	"flag transactional is set to 1 and prefix is pgoutput");

$result = $node_publisher->safe_psql(
	'postgres', qq(
		SELECT get_byte(data, 0)
		FROM pg_logical_slot_get_binary_changes('tap_sub', NULL, NULL,
			'proto_version', '1',
			'publication_names', 'tap_pub')
));

# no message and no BEGIN and COMMIT because of empty transaction optimization
is($result, qq(),
	'option messages defaults to false so message (M) is not available on slot'
);

$node_publisher->safe_psql('postgres', "INSERT INTO tab_test VALUES (1)");

my $message_lsn = $node_publisher->safe_psql('postgres',
	"SELECT pg_logical_emit_message(false, 'pgoutput', 'a non-transactional message')"
);

$node_publisher->safe_psql('postgres', "INSERT INTO tab_test VALUES (2)");

$result = $node_publisher->safe_psql(
	'postgres', qq(
		SELECT get_byte(data, 0), get_byte(data, 1)
		FROM pg_logical_slot_get_binary_changes('tap_sub', NULL, NULL,
			'proto_version', '1',
			'publication_names', 'tap_pub',
			'messages', 'true')
		WHERE lsn = '$message_lsn' AND xid = 0
));

is($result, qq(77|0), 'non-transactional message on slot is M');

# Ensure a non-transactional logical decoding message shows up on the slot when
# it is emitted within an aborted transaction. The message won't emit until
# something advances the LSN, hence, we intentionally forces the server to
# switch to a new WAL file.
$node_publisher->safe_psql(
	'postgres', qq(
		BEGIN;
		SELECT pg_logical_emit_message(false, 'pgoutput',
			'a non-transactional message is available even if the transaction is aborted 1');
		INSERT INTO tab_test VALUES (3);
		SELECT pg_logical_emit_message(true, 'pgoutput',
			'a transactional message is not available if the transaction is aborted');
		SELECT pg_logical_emit_message(false, 'pgoutput',
			'a non-transactional message is available even if the transaction is aborted 2');
		ROLLBACK;
		SELECT pg_switch_wal();
));

$result = $node_publisher->safe_psql(
	'postgres', qq(
		SELECT get_byte(data, 0), get_byte(data, 1)
		FROM pg_logical_slot_peek_binary_changes('tap_sub', NULL, NULL,
			'proto_version', '1',
			'publication_names', 'tap_pub',
			'messages', 'true')
));

is( $result, qq(77|0
77|0),
	'non-transactional message on slot from aborted transaction is M');

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
