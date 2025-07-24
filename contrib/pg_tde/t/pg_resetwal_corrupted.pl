
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Tests for handling a corrupted pg_control

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

unlink('/tmp/pg_resetwal_corrupted.per');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q{

# WAL Encryption
shared_preload_libraries = 'pg_tde'
});

$node->start;
$node->safe_psql('postgres', "CREATE EXTENSION pg_tde;");
$node->safe_psql('postgres',
	"SELECT pg_tde_add_global_key_provider_file('file-keyring-wal', '/tmp/pg_waldump_corrupted.per');"
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

my $pg_control = $node->data_dir . '/global/pg_control';
my $size = -s $pg_control;

# Read out the head of the file to get PG_CONTROL_VERSION in
# particular.
my $data;
open my $fh, '<', $pg_control or BAIL_OUT($!);
binmode $fh;
read $fh, $data, 16 or die $!;
close $fh;

# Fill pg_control with zeros
open $fh, '>', $pg_control or BAIL_OUT($!);
binmode $fh;
print $fh pack("x[$size]");
close $fh;

command_checks_all(
	[ 'pg_resetwal', '-n', $node->data_dir ],
	0,
	[qr/pg_control version number/],
	[
		qr/pg_resetwal: warning: pg_control exists but is broken or wrong version; ignoring it/
	],
	'processes corrupted pg_control all zeroes');

# Put in the previously saved header data.  This uses a different code
# path internally, allowing us to process a zero WAL segment size.
open $fh, '>', $pg_control or BAIL_OUT($!);
binmode $fh;
print $fh $data, pack("x[" . ($size - 16) . "]");
close $fh;

command_checks_all(
	[ 'pg_resetwal', '-n', $node->data_dir ],
	0,
	[qr/pg_control version number/],
	[
		qr/\Qpg_resetwal: warning: pg_control specifies invalid WAL segment size (0 bytes); proceed with caution\E/
	],
	'processes zero WAL segment size');

# now try to run it
command_fails_like(
	[ 'pg_resetwal', $node->data_dir ],
	qr/not proceeding because control file values were guessed/,
	'does not run when control file values were guessed');
command_ok([ 'pg_resetwal', '-f', $node->data_dir ],
	'runs with force when control file values were guessed');

done_testing();
