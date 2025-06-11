#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

unlink('/tmp/pg_tde_test_001_basic.per');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_tde'");
$node->start;

PGTDE::psql($node, 'postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;');

# Only whitelisted C or security definer functions are granted to public by default
PGTDE::psql(
	$node, 'postgres',
	q{
		SELECT
			pg_proc.oid::regprocedure
		FROM
			pg_catalog.pg_proc
			JOIN pg_catalog.pg_language ON prolang = pg_language.oid
			LEFT JOIN LATERAL aclexplode(proacl) ON TRUE
		WHERE
			proname LIKE 'pg_tde%' AND
			(lanname = 'c' OR prosecdef) AND
			(grantee IS NULL OR grantee = 0)
		ORDER BY pg_proc.oid::regprocedure::text;
	});

PGTDE::psql($node, 'postgres',
	"SELECT extname, extversion FROM pg_extension WHERE extname = 'pg_tde';");

PGTDE::psql($node, 'postgres',
	'CREATE TABLE test_enc (id SERIAL, k INTEGER, PRIMARY KEY (id)) USING tde_heap;'
);

PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/pg_tde_test_001_basic.per');"
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


# An encrypted table can be dropped even if we don't have access to the principal key.
$node->stop;
unlink('/tmp/pg_tde_test_001_basic.per');
$node->start;
PGTDE::psql($node, 'postgres', 'SELECT pg_tde_verify_key()');
PGTDE::psql($node, 'postgres', 'DROP TABLE test_enc;');

PGTDE::psql($node, 'postgres', 'DROP EXTENSION pg_tde;');

$node->stop;

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare, 0,
	"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files."
);

done_testing();
