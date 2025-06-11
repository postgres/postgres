#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

unlink('/tmp/rotate_key.per');
unlink('/tmp/rotate_key_2.per');
unlink('/tmp/rotate_key_2g.per');
unlink('/tmp/rotate_key_3.per');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_tde'");
$node->start;

PGTDE::psql($node, 'postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;');

PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/rotate_key.per');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_add_database_key_provider_file('file-2', '/tmp/rotate_key_2.per');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_add_global_key_provider_file('file-2', '/tmp/rotate_key_2g.per');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_add_global_key_provider_file('file-3', '/tmp/rotate_key_3.per');"
);

PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_list_all_database_key_providers();");

PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('test-db-key', 'file-vault');"
);

PGTDE::psql($node, 'postgres',
	'CREATE TABLE test_enc (id SERIAL, k INTEGER, PRIMARY KEY (id)) USING tde_heap;'
);

PGTDE::psql($node, 'postgres', 'INSERT INTO test_enc (k) VALUES (5), (6);');

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc ORDER BY id;');

# Rotate key
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('rotated-key1', 'file-vault');"
);
PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc ORDER BY id;');

PGTDE::append_to_result_file("-- server restart");
$node->restart;

PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_key_info();");
PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_server_key_info();"
);
PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc ORDER BY id;');

# Again rotate key
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('rotated-key2', 'file-2');"
);
PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc ORDER BY id;');

PGTDE::append_to_result_file("-- server restart");
$node->restart;

PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_key_info();");
PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_server_key_info();"
);
PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc ORDER BY id;');

# Again rotate key
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_global_key_provider('rotated-key', 'file-3', false);"
);
PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc ORDER BY id;');

PGTDE::append_to_result_file("-- server restart");
$node->restart;

PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_key_info();");
PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_server_key_info();"
);
PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc ORDER BY id;');

# TODO: add method to query current info
# And maybe debug tools to show what's in a file keyring?

# Again rotate key
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_global_key_provider('rotated-keyX', 'file-2', false);"
);
PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc ORDER BY id;');

PGTDE::append_to_result_file("-- server restart");
$node->restart;

PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_key_info();");
PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_server_key_info();"
);
PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc ORDER BY id;');

PGTDE::psql($node, 'postgres',
	'ALTER SYSTEM SET pg_tde.inherit_global_providers = off;');

# Things still work after a restart
PGTDE::append_to_result_file("-- server restart");
$node->restart;

# But now can't be changed to another global provider
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_global_key_provider('rotated-keyX2', 'file-2', false);"
);
PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_key_info();");
PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_server_key_info();"
);

PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('rotated-key2', 'file-2');"
);
PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_key_info();");
PGTDE::psql($node, 'postgres',
	"SELECT provider_id, provider_name, key_name FROM pg_tde_server_key_info();"
);

PGTDE::psql($node, 'postgres', 'DROP TABLE test_enc;');

PGTDE::psql($node, 'postgres',
	'ALTER SYSTEM RESET pg_tde.inherit_global_providers;');

PGTDE::append_to_result_file("-- server restart");
$node->restart;

PGTDE::psql($node, 'postgres', 'DROP EXTENSION pg_tde CASCADE;');

$node->stop;

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare, 0,
	"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files."
);

done_testing();
