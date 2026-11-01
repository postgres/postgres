
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

program_help_ok('pg_recvlogical');
program_version_ok('pg_recvlogical');
program_options_handling_ok('pg_recvlogical');

my $node = PostgreSQL::Test::Cluster->new('main');

# Initialize node without replication settings
$node->init(allows_streaming => 1, has_archiving => 1);
$node->append_conf(
	'postgresql.conf', q{
wal_level = 'logical'
max_replication_slots = 4
max_wal_senders = 4
log_min_messages = 'debug1'
log_error_verbosity = verbose
max_prepared_transactions = 10
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

$node->command_ok(
	[
		'pg_recvlogical',           '-S',
		'test',                     '-d',
		$node->connstr('postgres'), '--drop-slot'
	],
	'slot dropped');

# Check that pgoutput rejects options that require a parameter when passed
# without a value.
$node->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('pgoutput_slot', 'pgoutput')");

$node->command_fails_like(
	[
		'pg_recvlogical',
		'--slot' => 'pgoutput_slot',
		'--dbname' => $node->connstr('postgres'),
		'--start',
		'--option' => 'proto_version',
		'--no-loop',
		'--file' => '-',
	],
	qr/proto_version requires a parameter/,
	'pgoutput proto_version option without a value');

$node->command_fails_like(
	[
		'pg_recvlogical',
		'--slot' => 'pgoutput_slot',
		'--dbname' => $node->connstr('postgres'),
		'--start',
		'--option' => 'proto_version=1',
		'--option' => 'publication_names',
		'--no-loop',
		'--file' => '-',
	],
	qr/publication_names requires a parameter/,
	'pgoutput publication_names option without a value');

$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('pgoutput_slot')");

#test with two-phase option enabled
$node->command_ok(
	[
		'pg_recvlogical',           '-S',
		'test',                     '-d',
		$node->connstr('postgres'), '--create-slot',
		'--two-phase'
	],
	'slot with two-phase created');

$slot = $node->slot('test');
isnt($slot->{'restart_lsn'}, '', 'restart lsn is defined for new slot');

$node->safe_psql('postgres',
	"BEGIN; INSERT INTO test_table values (11); PREPARE TRANSACTION 'test'");
$node->safe_psql('postgres', "COMMIT PREPARED 'test'");
$nextlsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
chomp($nextlsn);

$node->command_fails(
	[
		'pg_recvlogical',           '-S',
		'test',                     '-d',
		$node->connstr('postgres'), '--start',
		'--endpos',                 "$nextlsn",
		'--two-phase',              '--no-loop',
		'-f',                       '-'
	],
	'incorrect usage');

$node->command_ok(
	[
		'pg_recvlogical', '-S', 'test', '-d', $node->connstr('postgres'),
		'--start', '--endpos', "$nextlsn", '--no-loop', '-f', '-'
	],
	'replayed a two-phase transaction');

done_testing();
