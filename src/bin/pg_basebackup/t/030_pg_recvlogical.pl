
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;
use Config;

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
$node->command_fails(
	[ 'pg_recvlogical', '--slot' => 'test' ],
	'pg_recvlogical needs a database');
$node->command_fails(
	[ 'pg_recvlogical', '--slot' => 'test', '--dbname' => 'postgres' ],
	'pg_recvlogical needs an action');
$node->command_fails(
	[
		'pg_recvlogical',
		'--slot' => 'test',
		'--dbname' => $node->connstr('postgres'),
		'--start',
	],
	'no destination file');
$node->command_fails_like(
	[ 'pg_recvlogical', '--startpos' => '123456789/0' ],
	qr/error: could not parse start position/,
	'start position with first component wider than 32 bits');
$node->command_fails_like(
	[ 'pg_recvlogical', '--startpos' => '0x1/0' ],
	qr/error: could not parse start position/,
	'start position with 0x prefix');
$node->command_fails_like(
	[ 'pg_recvlogical', '--endpos' => '0/123456789' ],
	qr/error: could not parse end position/,
	'end position with second component wider than 32 bits');
$node->command_fails_like(
	[ 'pg_recvlogical', '--endpos' => '1/2/3' ],
	qr/error: could not parse end position/,
	'end position with trailing garbage');

$node->command_ok(
	[
		'pg_recvlogical',
		'--slot' => 'test',
		'--dbname' => $node->connstr('postgres'),
		'--create-slot',
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
		'pg_recvlogical',
		'--slot' => 'test',
		'--dbname' => $node->connstr('postgres'),
		'--start',
		'--endpos' => $nextlsn,
		'--no-loop',
		'--file' => '-',
	],
	'replayed a transaction');

$node->command_ok(
	[
		'pg_recvlogical',
		'--slot' => 'test',
		'--dbname' => $node->connstr('postgres'),
		'--drop-slot'
	],
	'slot dropped');

#test with two-phase option enabled
$node->command_ok(
	[
		'pg_recvlogical',
		'--slot' => 'test',
		'--dbname' => $node->connstr('postgres'),
		'--create-slot',
		'--two-phase',
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
		'pg_recvlogical',
		'--slot' => 'test',
		'--dbname' => $node->connstr('postgres'),
		'--start',
		'--endpos' => $nextlsn,
		'--enable-two-phase', '--no-loop',
		'--file' => '-',
	],
	'incorrect usage');

$node->command_ok(
	[
		'pg_recvlogical',
		'--slot' => 'test',
		'--dbname' => $node->connstr('postgres'),
		'--start',
		'--endpos' => $nextlsn,
		'--no-loop',
		'--file' => '-',
	],
	'replayed a two-phase transaction');

$node->command_ok(
	[
		'pg_recvlogical',
		'--slot' => 'test',
		'--drop-slot'
	],
	'drop could work without dbname');

# test with failover option enabled
$node->command_ok(
	[
		'pg_recvlogical',
		'--slot' => 'test',
		'--dbname' => $node->connstr('postgres'),
		'--create-slot',
		'--enable-failover',
	],
	'slot with failover created');

my $result = $node->safe_psql('postgres',
	"SELECT failover FROM pg_catalog.pg_replication_slots WHERE slot_name = 'test'"
);
is($result, 't', "failover is enabled for the new slot");

# Test that when pg_recvlogical reconnects, it does not write duplicate
# records to the output file
my $outfile = $node->basedir . '/reconnect.out';

$node->command_ok(
	[
		'pg_recvlogical',
		'--slot' => 'reconnect_test',
		'--dbname' => $node->connstr('postgres'),
		'--create-slot',
	],
	'slot created for reconnection test');

# Insert the first record for this test
$node->safe_psql('postgres', 'INSERT INTO test_table VALUES (1)');

my @pg_recvlogical_cmd = (
	'pg_recvlogical',
	'--slot' => 'reconnect_test',
	'--dbname' => $node->connstr('postgres'),
	'--start',
	'--file' => $outfile,
	'--fsync-interval' => '1',
	'--status-interval' => '100',
	'--verbose');

# On Windows, specify --endpos so pg_recvlogical can terminate, since
# signals cannot be used. Use the current LSN plus 32MB as endpos, which
# would be sufficient to cover the WAL generated by the test INSERTs.
if ($Config{osname} eq 'MSWin32')
{
	$nextlsn = $node->safe_psql('postgres',
		"SELECT pg_current_wal_insert_lsn() + pg_size_bytes('32MB')");
	chomp($nextlsn);
	push(@pg_recvlogical_cmd, '--endpos' => $nextlsn);
}

my ($stdout, $stderr);
my $recv = IPC::Run::start(
	[@pg_recvlogical_cmd],
	'>' => \$stdout,
	'2>' => \$stderr);

# Wait for pg_recvlogical to receive and write the first INSERT
my $first_ins = wait_for_file($outfile, qr/INSERT/);

my $log_offset = -s $node->logfile;

# Terminate the walsender to force pg_recvlogical to reconnect
my $backend_pid = $node->safe_psql('postgres',
	"SELECT active_pid FROM pg_replication_slots WHERE slot_name = 'reconnect_test'"
);
$node->safe_psql('postgres', "SELECT pg_terminate_backend($backend_pid)");

# Wait for pg_recvlogical to reconnect
$node->wait_for_log(qr/acquired logical replication slot \"reconnect_test\"/,
	$log_offset);

# Insert the second record for this test
$node->safe_psql('postgres', 'INSERT INTO test_table VALUES (2)');

# Wait for pg_recvlogical to receive and write the second INSERT
wait_for_file($outfile, qr/INSERT/, $first_ins);

# Terminate pg_recvlogical by generating WAL until the current position
# reaches the specified --endpos on Windows, or by sending a TERM signal
# on other platforms.
if ($Config{osname} eq 'MSWin32')
{
	$node->poll_query_until('postgres',
		"SELECT pg_switch_wal() >= '$nextlsn' FROM pg_logical_emit_message(false, 'test', 'test')"
	) or die "Timed out while waiting for pg_recvlogical to end";
}
else
{
	$recv->signal('TERM');
}

$recv->finish();

my $outfiledata = slurp_file("$outfile");
my $count = (() = $outfiledata =~ /INSERT/g);
cmp_ok($count, '==', 2,
	'pg_recvlogical has received and written two INSERTs');

# Check that pg_recvlogical derives output file permissions from the source
# cluster.
SKIP:
{
	skip "unix-style permissions not supported on Windows", 2
	  if ($Config{osname} eq 'MSWin32' || $Config{osname} eq 'cygwin');

	# The cluster was initialized without group access, so pg_recvlogical
	# should create the output file as 0600 (-rw-------).
	my $mode = sprintf('%04o', (stat($outfile))[2] & 07777);
	is($mode, '0600',
		'pg_recvlogical output file has no group permissions (0600)');

	# Enable group access on the source cluster and its files, then restart
	# so pg_recvlogical observes the updated source cluster permissions.
	$node->stop;
	chmod_recursive($node->data_dir, 0750, 0640);
	$node->start;

	$outfile = $node->basedir . '/group_access.out';
	@pg_recvlogical_cmd = (
		'pg_recvlogical',
		'--slot' => 'reconnect_test',
		'--dbname' => $node->connstr('postgres'),
		'--start',
		'--file' => $outfile,
		'--fsync-interval' => '1');

	$recv = IPC::Run::start(
		[@pg_recvlogical_cmd],
		'>' => \$stdout,
		'2>' => \$stderr);

	$node->safe_psql('postgres', 'INSERT INTO test_table VALUES (3)');
	wait_for_file($outfile, qr/INSERT/);

	$recv->signal('TERM');
	$recv->finish();

	# With group access enabled on the source cluster, pg_recvlogical should
	# create the output file as 0640 (-rw-r-----).
	$mode = sprintf('%04o', (stat($outfile))[2] & 07777);
	is($mode, '0640',
		'pg_recvlogical output file respects group permissions (0640)');
}

SKIP:
{
	skip "signals not supported on Windows", 4
	  if ($Config{osname} eq 'MSWin32' || $Config{osname} eq 'cygwin');

	my $signal_outfile = $node->basedir . '/signal_shutdown.out';

	$node->command_ok(
		[
			'pg_recvlogical',
			'--slot' => 'signal_shutdown_test',
			'--dbname' => $node->connstr('postgres'),
			'--create-slot',
		],
		'slot created for signal shutdown test');

	@pg_recvlogical_cmd = (
		'pg_recvlogical',
		'--slot' => 'signal_shutdown_test',
		'--dbname' => $node->connstr('postgres'),
		'--start',
		'--file' => $signal_outfile,
		'--fsync-interval' => '100',
		'--status-interval' => '100');

	$recv = IPC::Run::start(
		[@pg_recvlogical_cmd],
		'>' => \$stdout,
		'2>' => \$stderr);

	$node->safe_psql('postgres', 'INSERT INTO test_table VALUES (42)');

	# Wait for not only INSERT but also COMMIT because the inserted
	# change might not yet be safely confirmable by final feedback until
	# the transaction has committed.
	wait_for_file($signal_outfile,
		qr/test_table: INSERT: x\[integer\]:42\b.*?\bCOMMIT\b/s);

	$recv->signal('TERM');
	$recv->finish();

	$nextlsn =
	  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	chomp($nextlsn);

	$node->command_ok(
		[
			'pg_recvlogical',
			'--slot' => 'signal_shutdown_test',
			'--dbname' => $node->connstr('postgres'),
			'--start',
			'--endpos' => $nextlsn,
			'--no-loop',
			'--file' => $signal_outfile,
		],
		'pg_recvlogical exits after signal without replaying flushed data');

	my $signal_data = slurp_file($signal_outfile);
	my $signal_count =
	  (() = $signal_data =~ /test_table: INSERT: x\[integer\]:42\b/g);
	is($signal_count, 1,
		'pg_recvlogical does not duplicate decoded changes after signal shutdown'
	);

	$node->command_ok(
		[
			'pg_recvlogical',
			'--slot' => 'signal_shutdown_test',
			'--dbname' => $node->connstr('postgres'),
			'--drop-slot'
		],
		'signal_shutdown_test slot dropped');
}

$node->command_ok(
	[
		'pg_recvlogical',
		'--slot' => 'reconnect_test',
		'--dbname' => $node->connstr('postgres'),
		'--drop-slot'
	],
	'reconnect_test slot dropped');

done_testing();
