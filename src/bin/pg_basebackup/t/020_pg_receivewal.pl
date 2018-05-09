use strict;
use warnings;
use TestLib;
use PostgresNode;
use Test::More tests => 19;

program_help_ok('pg_receivewal');
program_version_ok('pg_receivewal');
program_options_handling_ok('pg_receivewal');

# Set umask so test directories and files are created with default permissions
umask(0077);

my $primary = get_new_node('primary');
$primary->init(allows_streaming => 1);
$primary->start;

my $stream_dir = $primary->basedir . '/archive_wal';
mkdir($stream_dir);

# Sanity checks for command line options.
$primary->command_fails(['pg_receivewal'],
	'pg_receivewal needs target directory specified');
$primary->command_fails(
	[ 'pg_receivewal', '-D', $stream_dir, '--create-slot', '--drop-slot' ],
	'failure if both --create-slot and --drop-slot specified');
$primary->command_fails(
	[ 'pg_receivewal', '-D', $stream_dir, '--create-slot' ],
	'failure if --create-slot specified without --slot');
$primary->command_fails(
	[ 'pg_receivewal', '-D', $stream_dir, '--synchronous', '--no-sync' ],
	'failure if --synchronous specified with --no-sync');

# Slot creation and drop
my $slot_name = 'test';
$primary->command_ok(
	[ 'pg_receivewal', '--slot', $slot_name, '--create-slot' ],
	'creating a replication slot');
my $slot = $primary->slot($slot_name);
is($slot->{'slot_type'}, 'physical', 'physical replication slot was created');
is($slot->{'restart_lsn'}, '', 'restart LSN of new slot is null');
$primary->command_ok([ 'pg_receivewal', '--slot', $slot_name, '--drop-slot' ],
	'dropping a replication slot');
is($primary->slot($slot_name)->{'slot_type'},
	'', 'replication slot was removed');

# Generate some WAL.  Use --synchronous at the same time to add more
# code coverage.  Switch to the next segment first so that subsequent
# restarts of pg_receivewal will see this segment as full..
$primary->psql('postgres', 'CREATE TABLE test_table(x integer);');
$primary->psql('postgres', 'SELECT pg_switch_wal();');
my $nextlsn =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn();');
chomp($nextlsn);
$primary->psql('postgres',
	'INSERT INTO test_table VALUES (generate_series(1,100));');

# Stream up to the given position.
$primary->command_ok(
	[
		'pg_receivewal', '-D',     $stream_dir,     '--verbose',
		'--endpos',      $nextlsn, '--synchronous', '--no-loop'
	],
	'streaming some WAL with --synchronous');

# Permissions on WAL files should be default
SKIP:
{
	skip "unix-style permissions not supported on Windows", 1
	  if ($windows_os);

	ok(check_mode_recursive($stream_dir, 0700, 0600),
		"check stream dir permissions");
}
