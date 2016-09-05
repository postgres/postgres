# Testing of logical decoding using SQL interface and/or pg_recvlogical
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 2;

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->append_conf(
		'postgresql.conf', qq(
max_replication_slots = 4
wal_level = logical
));
$node_master->start;
my $backup_name = 'master_backup';

$node_master->safe_psql('postgres', qq[CREATE TABLE decoding_test(x integer, y text);]);

$node_master->safe_psql('postgres', qq[SELECT pg_create_logical_replication_slot('test_slot', 'test_decoding');]);

$node_master->safe_psql('postgres', qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(1,10) s;]);

# Basic decoding works
my($result) = $node_master->safe_psql('postgres', qq[SELECT pg_logical_slot_get_changes('test_slot', NULL, NULL);]);
is(scalar(split /^/m, $result), 12, 'Decoding produced 12 rows inc BEGIN/COMMIT');

# If we immediately crash the server we might lose the progress we just made
# and replay the same changes again. But a clean shutdown should never repeat
# the same changes when we use the SQL decoding interface.
$node_master->restart('fast');

# There are no new writes, so the result should be empty.
$result = $node_master->safe_psql('postgres', qq[SELECT pg_logical_slot_get_changes('test_slot', NULL, NULL);]);
chomp($result);
is($result, '', 'Decoding after fast restart repeats no rows');

# done with the node
$node_master->stop;
