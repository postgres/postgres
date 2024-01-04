
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_resetwal');
program_version_ok('pg_resetwal');
program_options_handling_ok('pg_resetwal');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', 'track_commit_timestamp = on');

command_like([ 'pg_resetwal', '-n', $node->data_dir ],
	qr/checkpoint/, 'pg_resetwal -n produces output');


# Permissions on PGDATA should be default
SKIP:
{
	skip "unix-style permissions not supported on Windows", 1
	  if ($windows_os);

	ok(check_mode_recursive($node->data_dir, 0700, 0600),
		'check PGDATA permissions');
}

command_ok([ 'pg_resetwal', '-D', $node->data_dir ], 'pg_resetwal runs');
$node->start;
is($node->safe_psql("postgres", "SELECT 1;"),
	1, 'server running and working after reset');

command_fails_like(
	[ 'pg_resetwal', $node->data_dir ],
	qr/lock file .* exists/,
	'fails if server running');

$node->stop('immediate');
command_fails_like(
	[ 'pg_resetwal', $node->data_dir ],
	qr/database server was not shut down cleanly/,
	'does not run after immediate shutdown');
command_ok(
	[ 'pg_resetwal', '-f', $node->data_dir ],
	'runs after immediate shutdown with force');
$node->start;
is($node->safe_psql("postgres", "SELECT 1;"),
	1, 'server running and working after forced reset');

$node->stop;

# check various command-line handling

# Note: This test intends to check that a nonexistent data directory
# gives a reasonable error message.  Because of the way the code is
# currently structured, you get an error about readings permissions,
# which is perhaps suboptimal, so feel free to update this test if
# this gets improved.
command_fails_like(
	[ 'pg_resetwal', 'foo' ],
	qr/error: could not read permissions of directory/,
	'fails with nonexistent data directory');

command_fails_like(
	[ 'pg_resetwal', 'foo', 'bar' ],
	qr/too many command-line arguments/,
	'fails with too many command-line arguments');

$ENV{PGDATA} = $node->data_dir;    # not used
command_fails_like(
	['pg_resetwal'],
	qr/no data directory specified/,
	'fails with too few command-line arguments');

# error cases
# -c
command_fails_like(
	[ 'pg_resetwal', '-c', 'foo', $node->data_dir ],
	qr/error: invalid argument for option -c/,
	'fails with incorrect -c option');
command_fails_like(
	[ 'pg_resetwal', '-c', '10,bar', $node->data_dir ],
	qr/error: invalid argument for option -c/,
	'fails with incorrect -c option part 2');
command_fails_like(
	[ 'pg_resetwal', '-c', '1,10', $node->data_dir ],
	qr/greater than/,
	'fails with -c value 1 part 1');
command_fails_like(
	[ 'pg_resetwal', '-c', '10,1', $node->data_dir ],
	qr/greater than/,
	'fails with -c value 1 part 2');
# -e
command_fails_like(
	[ 'pg_resetwal', '-e', 'foo', $node->data_dir ],
	qr/error: invalid argument for option -e/,
	'fails with incorrect -e option');
command_fails_like(
	[ 'pg_resetwal', '-e', '-1', $node->data_dir ],
	qr/must not be -1/,
	'fails with -e value -1');
# -l
command_fails_like(
	[ 'pg_resetwal', '-l', 'foo', $node->data_dir ],
	qr/error: invalid argument for option -l/,
	'fails with incorrect -l option');
# -m
command_fails_like(
	[ 'pg_resetwal', '-m', 'foo', $node->data_dir ],
	qr/error: invalid argument for option -m/,
	'fails with incorrect -m option');
command_fails_like(
	[ 'pg_resetwal', '-m', '10,bar', $node->data_dir ],
	qr/error: invalid argument for option -m/,
	'fails with incorrect -m option part 2');
command_fails_like(
	[ 'pg_resetwal', '-m', '0,10', $node->data_dir ],
	qr/must not be 0/,
	'fails with -m value 0 part 1');
command_fails_like(
	[ 'pg_resetwal', '-m', '10,0', $node->data_dir ],
	qr/must not be 0/,
	'fails with -m value 0 part 2');
# -o
command_fails_like(
	[ 'pg_resetwal', '-o', 'foo', $node->data_dir ],
	qr/error: invalid argument for option -o/,
	'fails with incorrect -o option');
command_fails_like(
	[ 'pg_resetwal', '-o', '0', $node->data_dir ],
	qr/must not be 0/,
	'fails with -o value 0');
# -O
command_fails_like(
	[ 'pg_resetwal', '-O', 'foo', $node->data_dir ],
	qr/error: invalid argument for option -O/,
	'fails with incorrect -O option');
command_fails_like(
	[ 'pg_resetwal', '-O', '-1', $node->data_dir ],
	qr/must not be -1/,
	'fails with -O value -1');
# --wal-segsize
command_fails_like(
	[ 'pg_resetwal', '--wal-segsize', 'foo', $node->data_dir ],
	qr/error: invalid value/,
	'fails with incorrect --wal-segsize option');
command_fails_like(
	[ 'pg_resetwal', '--wal-segsize', '13', $node->data_dir ],
	qr/must be a power/,
	'fails with invalid --wal-segsize value');
# -u
command_fails_like(
	[ 'pg_resetwal', '-u', 'foo', $node->data_dir ],
	qr/error: invalid argument for option -u/,
	'fails with incorrect -u option');
command_fails_like(
	[ 'pg_resetwal', '-u', '1', $node->data_dir ],
	qr/must be greater than/,
	'fails with -u value too small');
# -x
command_fails_like(
	[ 'pg_resetwal', '-x', 'foo', $node->data_dir ],
	qr/error: invalid argument for option -x/,
	'fails with incorrect -x option');
command_fails_like(
	[ 'pg_resetwal', '-x', '1', $node->data_dir ],
	qr/must be greater than/,
	'fails with -x value too small');

# run with control override options

my $out = (run_command([ 'pg_resetwal', '-n', $node->data_dir ]))[0];
$out =~ /^Database block size: *(\d+)$/m or die;
my $blcksz = $1;

my @cmd = ('pg_resetwal', '-D', $node->data_dir);

# some not-so-critical hardcoded values
push @cmd, '-e', 1;
push @cmd, '-l', '00000001000000320000004B';
push @cmd, '-o', 100_000;
push @cmd, '--wal-segsize', 1;

# these use the guidance from the documentation

sub get_slru_files
{
	opendir(my $dh, $node->data_dir . '/' . $_[0]) or die $!;
	my @files = sort grep { /[0-9A-F]+/ } readdir $dh;
	closedir $dh;
	return @files;
}

my (@files, $mult);

@files = get_slru_files('pg_commit_ts');
# XXX: Should there be a multiplier, similar to the other options?
# -c argument is "old,new"
push @cmd,
  '-c',
  sprintf("%d,%d", hex($files[0]) == 0 ? 3 : hex($files[0]), hex($files[-1]));

@files = get_slru_files('pg_multixact/offsets');
$mult = 32 * $blcksz / 4;
# -m argument is "new,old"
push @cmd, '-m',
  sprintf("%d,%d",
	(hex($files[-1]) + 1) * $mult,
	hex($files[0]) == 0 ? 1 : hex($files[0] * $mult));

@files = get_slru_files('pg_multixact/members');
$mult = 32 * int($blcksz / 20) * 4;
push @cmd, '-O', (hex($files[-1]) + 1) * $mult;

@files = get_slru_files('pg_xact');
$mult = 32 * $blcksz * 4;
push @cmd,
  '-u', (hex($files[0]) == 0 ? 3 : hex($files[0]) * $mult),
  '-x', ((hex($files[-1]) + 1) * $mult);

command_ok([ @cmd, '-n' ], 'runs with control override options, dry run');
command_ok(\@cmd, 'runs with control override options');
command_like(
	[ 'pg_resetwal', '-n', $node->data_dir ],
	qr/^Latest checkpoint's NextOID: *100000$/m,
	'spot check that control changes were applied');

$node->start;
ok(1, 'server started after reset');

done_testing();
