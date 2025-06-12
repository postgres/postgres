#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

unlink('/tmp/crash_recovery.per');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
checkpoint_timeout = 1h
shared_preload_libraries = 'pg_tde'
});
$node->start;

PGTDE::psql($node, 'postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;');
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_add_global_key_provider_file('global_keyring', '/tmp/crash_recovery.per');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_create_key_using_global_key_provider('wal_encryption_key', 'global_keyring');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_server_key_using_global_key_provider('wal_encryption_key', 'global_keyring');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_add_database_key_provider_file('db_keyring', '/tmp/crash_recovery.per');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_create_key_using_database_key_provider('db_key', 'db_keyring');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('db_key', 'db_keyring');"
);

PGTDE::psql($node, 'postgres',
	"CREATE TABLE test_enc (x int PRIMARY KEY) USING tde_heap;");
PGTDE::psql($node, 'postgres', "INSERT INTO test_enc (x) VALUES (1), (2);");

PGTDE::psql($node, 'postgres',
	"CREATE TABLE test_plain (x int PRIMARY KEY) USING heap;");
PGTDE::psql($node, 'postgres', "INSERT INTO test_plain (x) VALUES (3), (4);");

PGTDE::psql($node, 'postgres', "ALTER SYSTEM SET pg_tde.wal_encrypt = 'on';");

PGTDE::append_to_result_file("-- kill -9");
PGTDE::kill9_until_dead($node);

PGTDE::append_to_result_file("-- server start");
$node->start;

PGTDE::append_to_result_file("-- rotate wal key");
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_create_key_using_global_key_provider('wal_encryption_key_1', 'global_keyring');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_server_key_using_global_key_provider('wal_encryption_key_1', 'global_keyring');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_create_key_using_database_key_provider('db_key_1', 'db_keyring');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('db_key_1', 'db_keyring');"
);
PGTDE::psql($node, 'postgres', "INSERT INTO test_enc (x) VALUES (3), (4);");
PGTDE::append_to_result_file("-- kill -9");
PGTDE::kill9_until_dead($node);
PGTDE::append_to_result_file("-- server start");
PGTDE::append_to_result_file(
	"-- check that pg_tde_save_principal_key_redo hasn't destroyed a WAL key created during the server start"
);
$node->start;

PGTDE::append_to_result_file("-- rotate wal key");
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_create_key_using_global_key_provider('wal_encryption_key_2', 'global_keyring');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_server_key_using_global_key_provider('wal_encryption_key_2', 'global_keyring');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_create_key_using_database_key_provider('db_key_2', 'db_keyring');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('db_key_2', 'db_keyring');"
);
PGTDE::psql($node, 'postgres', "INSERT INTO test_enc (x) VALUES (5), (6);");
PGTDE::append_to_result_file("-- kill -9");
PGTDE::kill9_until_dead($node);
PGTDE::append_to_result_file("-- server start");
PGTDE::append_to_result_file(
	"-- check that the key rotation hasn't destroyed a WAL key created during the server start"
);
$node->start;

PGTDE::psql($node, 'postgres', "TABLE test_enc;");

PGTDE::psql($node, 'postgres',
	"CREATE TABLE test_enc2 (x int PRIMARY KEY) USING tde_heap;");
PGTDE::append_to_result_file("-- kill -9");
PGTDE::kill9_until_dead($node);
PGTDE::append_to_result_file("-- server start");
PGTDE::append_to_result_file(
	"-- check redo of the smgr internal key creation when the key is on disk"
);
$node->start;

$node->stop;

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare, 0,
	"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files."
);

done_testing();
