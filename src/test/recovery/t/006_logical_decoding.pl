# Testing of logical decoding using SQL interface and/or pg_recvlogical
#
# Most logical decoding tests are in contrib/test_decoding. This module
# is for work that doesn't fit well there, like where server restarts
# are required.
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 5;

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->append_conf(
		'postgresql.conf', qq(
wal_level = logical
));
$node_master->start;
my $backup_name = 'master_backup';

$node_master->safe_psql('postgres', qq[CREATE TABLE decoding_test(x integer, y text);]);

$node_master->safe_psql('postgres', qq[SELECT pg_create_logical_replication_slot('test_slot', 'test_decoding');]);

$node_master->safe_psql('postgres', qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(1,10) s;]);

# Basic decoding works
my($result) = $node_master->safe_psql('postgres', qq[SELECT pg_logical_slot_get_changes('test_slot', NULL, NULL);]);
is(scalar(my @foobar = split /^/m, $result), 12, 'Decoding produced 12 rows inc BEGIN/COMMIT');

# If we immediately crash the server we might lose the progress we just made
# and replay the same changes again. But a clean shutdown should never repeat
# the same changes when we use the SQL decoding interface.
$node_master->restart('fast');

# There are no new writes, so the result should be empty.
$result = $node_master->safe_psql('postgres', qq[SELECT pg_logical_slot_get_changes('test_slot', NULL, NULL);]);
chomp($result);
is($result, '', 'Decoding after fast restart repeats no rows');

# Insert some rows and verify that we get the same results from pg_recvlogical
# and the SQL interface.
$node_master->safe_psql('postgres', qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(1,4) s;]);

my $expected = q{BEGIN
table public.decoding_test: INSERT: x[integer]:1 y[text]:'1'
table public.decoding_test: INSERT: x[integer]:2 y[text]:'2'
table public.decoding_test: INSERT: x[integer]:3 y[text]:'3'
table public.decoding_test: INSERT: x[integer]:4 y[text]:'4'
COMMIT};

my $stdout_sql = $node_master->safe_psql('postgres', qq[SELECT data FROM pg_logical_slot_peek_changes('test_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');]);
is($stdout_sql, $expected, 'got expected output from SQL decoding session');

my $endpos = $node_master->safe_psql('postgres', "SELECT location FROM pg_logical_slot_peek_changes('test_slot', NULL, NULL) ORDER BY location DESC LIMIT 1;");
diag "waiting to replay $endpos";

my $stdout_recv = $node_master->pg_recvlogical_upto('postgres', 'test_slot', $endpos, 10, 'include-xids' => '0', 'skip-empty-xacts' => '1');
chomp($stdout_recv);
is($stdout_recv, $expected, 'got same expected output from pg_recvlogical decoding session');

$stdout_recv = $node_master->pg_recvlogical_upto('postgres', 'test_slot', $endpos, 10, 'include-xids' => '0', 'skip-empty-xacts' => '1');
chomp($stdout_recv);
is($stdout_recv, '', 'pg_recvlogical acknowledged changes, nothing pending on slot');

# done with the node
$node_master->stop;
