use strict;
use warnings FATAL => 'all';
use Config;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_basebackup');
program_version_ok('pg_basebackup');
program_options_handling_ok('pg_basebackup');

my $tempdir = PostgreSQL::Test::Utils::tempdir;

my $node = PostgreSQL::Test::Cluster->new('main');

# Initialize node without replication settings
$node->init(
	allows_streaming => 1,
	extra => ['--data-checksums'],
	auth_extra => [ '--create-role', 'backupuser' ]);
$node->start;

# Sanity checks for options with WAL encryption
$node->command_fails_like(
	[ 'pg_basebackup', '-D', "$tempdir/backup", '-E', '-Ft' ],
	qr/can not encrypt WAL in tar mode/,
	'encryption in tar mode');

$node->command_fails_like(
	[ 'pg_basebackup', '-D', "$tempdir/backup", '-E', '-X', 'fetch' ],
	qr/WAL encryption can only be used with WAL streaming/,
	'encryption with WAL fetch');

$node->command_fails_like(
	[ 'pg_basebackup', '-D', "$tempdir/backup", '-E', '-X', 'none' ],
	qr/WAL encryption can only be used with WAL streaming/,
	'encryption with WAL none');

$node->command_fails_like(
	[ 'pg_basebackup', '-D', "$tempdir/backup", '-E' ],
	qr/could not find server principal key/,
	'encryption with no pg_tde dir');

$node->command_fails_like(
	[ 'pg_basebackup', '-D', "$tempdir/backup", '--encrypt-wal' ],
	qr/could not find server principal key/,
	'encryption with no pg_tde dir long flag');

done_testing();
