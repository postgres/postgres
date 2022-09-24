
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

program_help_ok('pg_receivewal');
program_version_ok('pg_receivewal');
program_options_handling_ok('pg_receivewal');

# Set umask so test directories and files are created with default permissions
umask(0077);

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1, extra => ['--wal-segsize=1']);
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
$primary->command_fails_like(
	[ 'pg_receivewal', '-D', $stream_dir, '--compress', 'none:1', ],
	qr/\Qpg_receivewal: error: invalid compression specification: compression algorithm "none" does not accept a compression level/,
	'failure if --compress none:N (where N > 0)');

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
$primary->psql('postgres', 'CREATE TABLE test_table(x integer PRIMARY KEY);');
$primary->psql('postgres', 'SELECT pg_switch_wal();');
my $nextlsn =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn();');
chomp($nextlsn);
$primary->psql('postgres', 'INSERT INTO test_table VALUES (1);');

# Stream up to the given position.  This is necessary to have a fixed
# started point for the next commands done in this test, with or without
# compression involved.
$primary->command_ok(
	[
		'pg_receivewal', '-D',     $stream_dir,     '--verbose',
		'--endpos',      $nextlsn, '--synchronous', '--no-loop'
	],
	'streaming some WAL with --synchronous');

# Verify that one partial file was generated and keep track of it
my @partial_wals = glob "$stream_dir/*\.partial";
is(scalar(@partial_wals), 1, "one partial WAL segment was created");

note "Testing pg_receivewal with compression methods";

# Check ZLIB compression if available.
SKIP:
{
	skip "postgres was not built with ZLIB support", 5
	  if (!check_pg_config("#define HAVE_LIBZ 1"));

	# Generate more WAL worth one completed, compressed, segment.
	$primary->psql('postgres', 'SELECT pg_switch_wal();');
	$nextlsn =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn();');
	chomp($nextlsn);
	$primary->psql('postgres', 'INSERT INTO test_table VALUES (2);');

	$primary->command_ok(
		[
			'pg_receivewal', '-D',     $stream_dir,  '--verbose',
			'--endpos',      $nextlsn, '--compress', 'gzip:1',
			'--no-loop'
		],
		"streaming some WAL using ZLIB compression");

	# Verify that the stored files are generated with their expected
	# names.
	my @zlib_wals = glob "$stream_dir/*.gz";
	is(scalar(@zlib_wals), 1,
		"one WAL segment compressed with ZLIB was created");
	my @zlib_partial_wals = glob "$stream_dir/*.gz.partial";
	is(scalar(@zlib_partial_wals),
		1, "one partial WAL segment compressed with ZLIB was created");

	# Verify that the start streaming position is computed correctly by
	# comparing it with the partial file generated previously.  The name
	# of the previous partial, now-completed WAL segment is updated, keeping
	# its base number.
	$partial_wals[0] =~ s/\.partial$/.gz/;
	is($zlib_wals[0] eq $partial_wals[0],
		1, "one partial WAL segment is now completed");
	# Update the list of partial wals with the current one.
	@partial_wals = @zlib_partial_wals;

	# Check the integrity of the completed segment, if gzip is a command
	# available.
	my $gzip = $ENV{GZIP_PROGRAM};
	skip "program gzip is not found in your system", 1
	  if (!defined $gzip
		|| $gzip eq '');

	my $gzip_is_valid = system_log($gzip, '--test', @zlib_wals);
	is($gzip_is_valid, 0,
		"gzip verified the integrity of compressed WAL segments");
}

# Check LZ4 compression if available
SKIP:
{
	skip "postgres was not built with LZ4 support", 5
	  if (!check_pg_config("#define USE_LZ4 1"));

	# Generate more WAL including one completed, compressed segment.
	$primary->psql('postgres', 'SELECT pg_switch_wal();');
	$nextlsn =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn();');
	chomp($nextlsn);
	$primary->psql('postgres', 'INSERT INTO test_table VALUES (3);');

	# Stream up to the given position.
	$primary->command_ok(
		[
			'pg_receivewal', '-D',     $stream_dir, '--verbose',
			'--endpos',      $nextlsn, '--no-loop', '--compress',
			'lz4'
		],
		'streaming some WAL using --compress=lz4');

	# Verify that the stored files are generated with their expected
	# names.
	my @lz4_wals = glob "$stream_dir/*.lz4";
	is(scalar(@lz4_wals), 1,
		"one WAL segment compressed with LZ4 was created");
	my @lz4_partial_wals = glob "$stream_dir/*.lz4.partial";
	is(scalar(@lz4_partial_wals),
		1, "one partial WAL segment compressed with LZ4 was created");

	# Verify that the start streaming position is computed correctly by
	# comparing it with the partial file generated previously.  The name
	# of the previous partial, now-completed WAL segment is updated, keeping
	# its base number.
	$partial_wals[0] =~ s/(\.gz)?\.partial$/.lz4/;
	is($lz4_wals[0] eq $partial_wals[0],
		1, "one partial WAL segment is now completed");
	# Update the list of partial wals with the current one.
	@partial_wals = @lz4_partial_wals;

	# Check the integrity of the completed segment, if LZ4 is an available
	# command.
	my $lz4 = $ENV{LZ4};
	skip "program lz4 is not found in your system", 1
	  if (!defined $lz4
		|| $lz4 eq '');

	my $lz4_is_valid = system_log($lz4, '-t', @lz4_wals);
	is($lz4_is_valid, 0,
		"lz4 verified the integrity of compressed WAL segments");
}

# Verify that the start streaming position is computed and that the value is
# correct regardless of whether any compression is available.
$primary->psql('postgres', 'SELECT pg_switch_wal();');
$nextlsn =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn();');
chomp($nextlsn);
$primary->psql('postgres', 'INSERT INTO test_table VALUES (4);');
$primary->command_ok(
	[
		'pg_receivewal', '-D',     $stream_dir, '--verbose',
		'--endpos',      $nextlsn, '--no-loop'
	],
	"streaming some WAL");

$partial_wals[0] =~ s/(\.gz|\.lz4)?.partial//;
ok(-e $partial_wals[0], "check that previously partial WAL is now complete");

# Permissions on WAL files should be default
SKIP:
{
	skip "unix-style permissions not supported on Windows", 1
	  if ($windows_os);

	ok(check_mode_recursive($stream_dir, 0700, 0600),
		"check stream dir permissions");
}

note "Testing pg_receivewal with slot as starting streaming point";

# When using a replication slot, archiving should be resumed from the slot's
# restart LSN.  Use a new archive location and new slot for this test.
my $slot_dir = $primary->basedir . '/slot_wal';
mkdir($slot_dir);
$slot_name = 'archive_slot';

# Setup the slot, reserving WAL at creation (corresponding to the
# last redo LSN here, actually, so use a checkpoint to reduce the
# number of segments archived).
$primary->psql('postgres', 'checkpoint;');
$primary->psql('postgres',
	"SELECT pg_create_physical_replication_slot('$slot_name', true);");

# Get the segment name associated with the slot's restart LSN, that should
# be archived.
my $walfile_streamed = $primary->safe_psql(
	'postgres',
	"SELECT pg_walfile_name(restart_lsn)
  FROM pg_replication_slots
  WHERE slot_name = '$slot_name';");

# Switch to a new segment, to make sure that the segment retained by the
# slot is still streamed.  This may not be necessary, but play it safe.
$primary->psql('postgres', 'INSERT INTO test_table VALUES (5);');
$primary->psql('postgres', 'SELECT pg_switch_wal();');
$nextlsn =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn();');
chomp($nextlsn);

# Add a bit more data to accelerate the end of the next pg_receivewal
# commands.
$primary->psql('postgres', 'INSERT INTO test_table VALUES (6);');

# Check case where the slot does not exist.
$primary->command_fails_like(
	[
		'pg_receivewal',   '-D', $slot_dir,   '--slot',
		'nonexistentslot', '-n', '--no-sync', '--verbose',
		'--endpos',        $nextlsn
	],
	qr/pg_receivewal: error: replication slot "nonexistentslot" does not exist/,
	'pg_receivewal fails with non-existing slot');
$primary->command_ok(
	[
		'pg_receivewal', '-D', $slot_dir,   '--slot',
		$slot_name,      '-n', '--no-sync', '--verbose',
		'--endpos',      $nextlsn
	],
	"WAL streamed from the slot's restart_lsn");
ok(-e "$slot_dir/$walfile_streamed",
	"WAL from the slot's restart_lsn has been archived");

# Test timeline switch using a replication slot, requiring a promoted
# standby.
my $backup_name = "basebackup";
$primary->backup($backup_name);
my $standby = PostgreSQL::Test::Cluster->new("standby");
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;

# Create a replication slot on this new standby
my $archive_slot = "archive_slot";
$standby->psql(
	'',
	"CREATE_REPLICATION_SLOT $archive_slot PHYSICAL (RESERVE_WAL)",
	replication => 1);
# Wait for standby catchup
$primary->wait_for_catchup($standby);
# Get a walfilename from before the promotion to make sure it is archived
# after promotion
my $standby_slot         = $standby->slot($archive_slot);
my $replication_slot_lsn = $standby_slot->{'restart_lsn'};

# pg_walfile_name() is not supported while in recovery, so use the primary
# to build the segment name.  Both nodes are on the same timeline, so this
# produces a segment name with the timeline we are switching from.
my $walfile_before_promotion =
  $primary->safe_psql('postgres',
	"SELECT pg_walfile_name('$replication_slot_lsn');");
# Everything is setup, promote the standby to trigger a timeline switch.
$standby->promote;

# Force a segment switch to make sure at least one full WAL is archived
# on the new timeline.
my $walfile_after_promotion = $standby->safe_psql('postgres',
	"SELECT pg_walfile_name(pg_current_wal_insert_lsn());");
$standby->psql('postgres', 'INSERT INTO test_table VALUES (7);');
$standby->psql('postgres', 'SELECT pg_switch_wal();');
$nextlsn =
  $standby->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn();');
chomp($nextlsn);
# This speeds up the operation.
$standby->psql('postgres', 'INSERT INTO test_table VALUES (8);');

# Now try to resume from the slot after the promotion.
my $timeline_dir = $primary->basedir . '/timeline_wal';
mkdir($timeline_dir);

$standby->command_ok(
	[
		'pg_receivewal', '-D',     $timeline_dir, '--verbose',
		'--endpos',      $nextlsn, '--slot',      $archive_slot,
		'--no-sync',     '-n'
	],
	"Stream some wal after promoting, resuming from the slot's position");
ok(-e "$timeline_dir/$walfile_before_promotion",
	"WAL segment $walfile_before_promotion archived after timeline jump");
ok(-e "$timeline_dir/$walfile_after_promotion",
	"WAL segment $walfile_after_promotion archived after timeline jump");
ok(-e "$timeline_dir/00000002.history",
	"timeline history file archived after timeline jump");

done_testing();
