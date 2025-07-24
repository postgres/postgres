
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

unlink('/tmp/pg_resetwal_basic.per');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
track_commit_timestamp = on

# WAL Encryption
shared_preload_libraries = 'pg_tde'
});

$node->start;
$node->safe_psql('postgres', "CREATE EXTENSION pg_tde;");
$node->safe_psql('postgres',
	"SELECT pg_tde_add_global_key_provider_file('file-keyring-wal', '/tmp/pg_resetwal_basic.per');"
);
$node->safe_psql('postgres',
	"SELECT pg_tde_create_key_using_global_key_provider('server-key', 'file-keyring-wal');"
);
$node->safe_psql('postgres',
	"SELECT pg_tde_set_server_key_using_global_key_provider('server-key', 'file-keyring-wal');"
);

$node->append_conf(
	'postgresql.conf', q{
pg_tde.wal_encrypt = on
});
$node->stop;


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
