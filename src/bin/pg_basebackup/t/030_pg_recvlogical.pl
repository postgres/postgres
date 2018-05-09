use strict;
use warnings;
use TestLib;
use PostgresNode;
use Test::More tests => 15;

program_help_ok('pg_recvlogical');
program_version_ok('pg_recvlogical');
program_options_handling_ok('pg_recvlogical');

my $node = get_new_node('main');

# Initialize node without replication settings
$node->init(allows_streaming => 1, has_archiving => 1);
$node->append_conf(
	'postgresql.conf', q{
wal_level = 'logical'
max_replication_slots = 4
max_wal_senders = 4
log_min_messages = 'debug1'
log_error_verbosity = verbose
});
$node->dump_info;
$node->start;

$node->command_fails(['pg_recvlogical'], 'pg_recvlogical needs a slot name');
$node->command_fails([ 'pg_recvlogical', '-S', 'test' ],
	'pg_recvlogical needs a database');
$node->command_fails([ 'pg_recvlogical', '-S', 'test', '-d', 'postgres' ],
	'pg_recvlogical needs an action');
$node->command_fails(
	[
		'pg_recvlogical',           '-S',
		'test',                     '-d',
		$node->connstr('postgres'), '--start'
	],
	'no destination file');

$node->command_ok(
	[
		'pg_recvlogical',           '-S',
		'test',                     '-d',
		$node->connstr('postgres'), '--create-slot'
	],
	'slot created');

my $slot = $node->slot('test');
isnt($slot->{'restart_lsn'}, '', 'restart lsn is defined for new slot');

$node->psql('postgres', 'CREATE TABLE test_table(x integer)');
$node->psql('postgres',
	'INSERT INTO test_table(x) SELECT y FROM generate_series(1, 10) a(y);');
my $nextlsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
chomp($nextlsn);

$node->command_ok(
	[
		'pg_recvlogical', '-S', 'test', '-d', $node->connstr('postgres'),
		'--start', '--endpos', "$nextlsn", '--no-loop', '-f', '-'
	],
	'replayed a transaction');
