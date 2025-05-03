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
$node->start;

PGTDE::psql($node, 'postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;');

PGTDE::psql($node, 'postgres',
	"SELECT extname, extversion FROM pg_extension WHERE extname = 'pg_tde';");

PGTDE::psql($node, 'postgres',
	'CREATE TABLE test_enc (id SERIAL, k INTEGER, PRIMARY KEY (id)) USING tde_heap;'
);

PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/pg_tde_test_keyring.per');"
);

PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('test-db-key', 'file-vault');"
);

PGTDE::psql($node, 'postgres',
	'CREATE TABLE test_enc (id SERIAL, k VARCHAR(32), PRIMARY KEY (id)) USING tde_heap;'
);

PGTDE::psql($node, 'postgres',
	"INSERT INTO test_enc (k) VALUES ('foobar'), ('barfoo');");

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc ORDER BY id;');

PGTDE::append_to_result_file("-- server restart");
$node->restart;

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc ORDER BY id;');

# Verify that we can't see the data in the file
my $tablefile = $node->data_dir . '/'
  . $node->safe_psql('postgres', "SELECT pg_relation_filepath('test_enc');");

PGTDE::append_to_result_file(
	'TABLEFILE FOUND: ' . (-f $tablefile ? 'yes' : 'no'));

my $strings = 'CONTAINS FOO (should be empty): ';
$strings .= `strings $tablefile | grep foo`;
PGTDE::append_to_result_file($strings);

PGTDE::psql($node, 'postgres', 'DROP TABLE test_enc;');

PGTDE::psql($node, 'postgres', 'DROP EXTENSION pg_tde;');

$node->stop;

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare, 0,
	"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files."
);

done_testing();
