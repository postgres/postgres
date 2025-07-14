#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

unlink('/tmp/wal_archiving.per');

# Test archive_command

my $primary = PostgreSQL::Test::Cluster->new('primary');
my $archive_dir = $primary->archive_dir;
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"shared_preload_libraries = 'pg_tde'");
$primary->append_conf('postgresql.conf', "wal_level = 'replica'");
$primary->append_conf('postgresql.conf', "autovacuum = off");
$primary->append_conf('postgresql.conf', "checkpoint_timeout = 1h");
$primary->append_conf('postgresql.conf', "archive_mode = on");
$primary->append_conf('postgresql.conf',
	"archive_command = 'pg_tde_archive_decrypt %p cp %p $archive_dir/%f'");
$primary->start;

$primary->safe_psql('postgres', "CREATE EXTENSION pg_tde;");

$primary->safe_psql('postgres',
	"SELECT pg_tde_add_global_key_provider_file('keyring', '/tmp/wal_archiving.per');"
);
$primary->safe_psql('postgres',
	"SELECT pg_tde_create_key_using_global_key_provider('server-key', 'keyring');"
);
$primary->safe_psql('postgres',
	"SELECT pg_tde_set_server_key_using_global_key_provider('server-key', 'keyring');"
);

$primary->append_conf('postgresql.conf', "pg_tde.wal_encrypt = on");

$primary->backup('backup', backup_options => [ '-X', 'none' ]);

$primary->safe_psql('postgres',
	"CREATE TABLE t1 AS SELECT 'foobar_plain' AS x");

$primary->restart;

$primary->safe_psql('postgres',
	"CREATE TABLE t2 AS SELECT 'foobar_enc' AS x");

my $data_dir = $primary->data_dir;

like(
	`strings $data_dir/pg_wal/0000000100000000000000??`,
	qr/foobar_plain/,
	'should find foobar_plain in WAL');
unlike(
	`strings $data_dir/pg_wal/0000000100000000000000??`,
	qr/foobar_enc/,
	'should not find foobar_enc in WAL');

$primary->stop;

like(
	`strings $archive_dir/0000000100000000000000??`,
	qr/foobar_plain/,
	'should find foobar_plain in archive');
like(
	`strings $archive_dir/0000000100000000000000??`,
	qr/foobar_enc/,
	'should find foobar_enc in archive');

# Test restore_command

my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->init_from_backup($primary, 'backup');
$replica->append_conf('postgresql.conf',
	"restore_command = 'pg_tde_restore_encrypt %f %p cp $archive_dir/%f %p'");
$replica->append_conf('postgresql.conf', "recovery_target_action = promote");
$replica->set_recovery_mode;
$replica->start;

$data_dir = $replica->data_dir;

unlike(
	`strings $data_dir/pg_wal/0000000100000000000000??`,
	qr/foobar_plain/,
	'should not find foobar_plain in WAL since it is encrypted');
unlike(
	`strings $data_dir/pg_wal/0000000100000000000000??`,
	qr/foobar_enc/,
	'should not find foobar_enc in WAL since it is encrypted');

my $result = $replica->safe_psql('postgres',
	'SELECT * FROM t1 UNION ALL SELECT * FROM t2');

is($result, "foobar_plain\nfoobar_enc", 'b');

$replica->stop;

done_testing();
