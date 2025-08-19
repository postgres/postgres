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
	"archive_command = 'pg_tde_archive_decrypt %f %p \"cp %%p $archive_dir/%%f\"'"
);
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

# This is a quite ugly dance to make sure we have a replica starting in a stats
# with encrypted WAL and without. We do this by taking a base backup while
# encryption is disabled and one where it is enabled.
#
# We also generate some plaintext WAL and some ecrnypted WAL.

$primary->backup('plain_wal', backup_options => [ '-X', 'none' ]);

$primary->append_conf('postgresql.conf', "pg_tde.wal_encrypt = on");

$primary->restart;

$primary->backup('enc_wal', backup_options => [ '-X', 'none' ]);

$primary->append_conf('postgresql.conf', "pg_tde.wal_encrypt = off");

$primary->restart;

$primary->append_conf('postgresql.conf', "pg_tde.wal_encrypt = on");

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
	'should not find foobar_enc in WAL since it is encrypted');

$primary->stop;

like(
	`strings $archive_dir/0000000100000000000000??`,
	qr/foobar_plain/,
	'should find foobar_plain in archive');
like(
	`strings $archive_dir/0000000100000000000000??`,
	qr/foobar_enc/,
	'should find foobar_enc in archive');

# Test restore_command with encrypted WAL

my $replica_enc = PostgreSQL::Test::Cluster->new('replica_enc');
$replica_enc->init_from_backup($primary, 'enc_wal');
$replica_enc->append_conf('postgresql.conf',
	"restore_command = 'pg_tde_restore_encrypt %f %p \"cp $archive_dir/%%f %%p\"'"
);
$replica_enc->set_standby_mode;
$replica_enc->start;

$replica_enc->wait_for_log("waiting for WAL to become available");

$data_dir = $replica_enc->data_dir;

unlike(
	`strings $data_dir/pg_wal/0000000100000000000000??`,
	qr/foobar_plain/,
	'should not find foobar_plain in WAL since it is encrypted');
unlike(
	`strings $data_dir/pg_wal/0000000100000000000000??`,
	qr/foobar_enc/,
	'should not find foobar_enc in WAL since it is encrypted');

$replica_enc->promote;

my $result = $replica_enc->safe_psql('postgres',
	'SELECT * FROM t1 UNION ALL SELECT * FROM t2');

is($result, "foobar_plain\nfoobar_enc", 'b');

$replica_enc->stop;

# Test restore_command with plain WAL

my $replica_plain = PostgreSQL::Test::Cluster->new('replica_plain');
$replica_plain->init_from_backup($primary, 'plain_wal');
$replica_plain->append_conf('postgresql.conf',
	"restore_command = 'pg_tde_restore_encrypt %f %p \"cp $archive_dir/%%f %%p\"'"
);
$replica_plain->set_standby_mode;
$replica_plain->start;

$replica_plain->wait_for_log("waiting for WAL to become available");

$data_dir = $replica_plain->data_dir;

like(
	`strings $data_dir/pg_wal/0000000100000000000000??`,
	qr/foobar_plain/,
	'should find foobar_plain in WAL since it is not encrypted');
like(
	`strings $data_dir/pg_wal/0000000100000000000000??`,
	qr/foobar_enc/,
	'should find foobar_enc in WAL since it is not encrypted');

$replica_plain->promote;

$result = $replica_plain->safe_psql('postgres',
	'SELECT * FROM t1 UNION ALL SELECT * FROM t2');

is($result, "foobar_plain\nfoobar_enc", 'b');

$replica_plain->stop;

done_testing();
