#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_tde'");
$node->append_conf('postgresql.conf', "wal_level = 'logical'");
# NOT testing that it can't start: the test framework doesn't have an easy way to do this
#$node->append_conf('postgresql.conf', "pg_tde.wal_encrypt = 1"});
$node->start;

PGTDE::psql($node, 'postgres', "CREATE EXTENSION IF NOT EXISTS pg_tde;");

PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_add_global_key_provider_file('file-keyring-010', '/tmp/pg_tde_test_keyring010.per');"
);

PGTDE::psql($node, 'postgres', 'SELECT pg_tde_verify_server_key();');

PGTDE::psql($node, 'postgres',
	'SELECT key_name, key_provider_name, key_provider_id FROM pg_tde_server_key_info();'
);

PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_server_key_using_global_key_provider('server-key', 'file-keyring-010');"
);

PGTDE::psql($node, 'postgres', 'SELECT pg_tde_verify_server_key();');

PGTDE::psql($node, 'postgres',
	'SELECT key_name, key_provider_name, key_provider_id FROM pg_tde_server_key_info();'
);

PGTDE::psql($node, 'postgres', 'ALTER SYSTEM SET pg_tde.wal_encrypt = on;');

PGTDE::append_to_result_file("-- server restart with wal encryption");
$node->restart;

PGTDE::psql($node, 'postgres', "SHOW pg_tde.wal_encrypt;");

PGTDE::psql($node, 'postgres',
	"SELECT slot_name FROM pg_create_logical_replication_slot('tde_slot', 'test_decoding');"
);

PGTDE::psql($node, 'postgres',
	'CREATE TABLE test_wal (id SERIAL, k INTEGER, PRIMARY KEY (id));');

PGTDE::psql($node, 'postgres', 'INSERT INTO test_wal (k) VALUES (1), (2);');

PGTDE::psql($node, 'postgres', 'ALTER SYSTEM SET pg_tde.wal_encrypt = off;');

PGTDE::append_to_result_file("-- server restart without wal encryption");
$node->restart;

PGTDE::psql($node, 'postgres', "SHOW pg_tde.wal_encrypt;");

PGTDE::psql($node, 'postgres', 'INSERT INTO test_wal (k) VALUES (3), (4);');

PGTDE::psql($node, 'postgres', 'ALTER SYSTEM SET pg_tde.wal_encrypt = on;');

PGTDE::append_to_result_file("-- server restart with wal encryption");
$node->restart;

PGTDE::psql($node, 'postgres', "SHOW pg_tde.wal_encrypt;");

PGTDE::psql($node, 'postgres', 'INSERT INTO test_wal (k) VALUES (5), (6);');

PGTDE::append_to_result_file("-- server restart with still wal encryption");
$node->restart;

PGTDE::psql($node, 'postgres', "SHOW pg_tde.wal_encrypt;");

PGTDE::psql($node, 'postgres', 'INSERT INTO test_wal (k) VALUES (7), (8);');

PGTDE::psql($node, 'postgres',
	"SELECT data FROM pg_logical_slot_get_changes('tde_slot', NULL, NULL);");

PGTDE::psql($node, 'postgres',
	"SELECT pg_drop_replication_slot('tde_slot');");

PGTDE::psql($node, 'postgres', 'DROP EXTENSION pg_tde;');

$node->stop;

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare, 0,
	"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files."
);

done_testing();
