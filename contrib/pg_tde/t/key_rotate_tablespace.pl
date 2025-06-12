#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

unlink('/tmp/key_rotate_tablespace.per');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_tde'");
$node->start;

PGTDE::psql($node, 'postgres',
	"SET allow_in_place_tablespaces = true; CREATE TABLESPACE test_tblspace LOCATION '';"
);
PGTDE::psql($node, 'postgres',
	'CREATE DATABASE tbc TABLESPACE = test_tblspace;');

PGTDE::psql($node, 'tbc', 'CREATE EXTENSION IF NOT EXISTS pg_tde;');
PGTDE::psql($node, 'tbc',
	"SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/key_rotate_tablespace.per');"
);
PGTDE::psql($node, 'tbc',
	"SELECT pg_tde_create_key_using_database_key_provider('test-db-key', 'file-vault');"
);
PGTDE::psql($node, 'tbc',
	"SELECT pg_tde_set_key_using_database_key_provider('test-db-key', 'file-vault');"
);

PGTDE::psql(
	$node, 'tbc', "
CREATE TABLE country_table (
     country_id   serial primary key,
     country_name text unique not null,
     continent    text not null
) USING tde_heap;
");

PGTDE::psql(
	$node, 'tbc', "
INSERT INTO country_table (country_name, continent)
     VALUES ('Japan', 'Asia'),
            ('UK', 'Europe'),
            ('USA', 'North America');
");

PGTDE::psql($node, 'tbc', 'SELECT * FROM country_table;');
PGTDE::psql($node, 'tbc',
	"SELECT pg_tde_create_key_using_database_key_provider('new-k', 'file-vault');"
);
PGTDE::psql($node, 'tbc',
	"SELECT pg_tde_set_key_using_database_key_provider('new-k', 'file-vault');"
);

PGTDE::append_to_result_file("-- server restart");
$node->restart;

PGTDE::psql($node, 'tbc', 'SELECT * FROM country_table;');

PGTDE::psql($node, 'tbc', 'DROP EXTENSION pg_tde CASCADE;');

PGTDE::psql($node, 'postgres', 'DROP DATABASE tbc;');
PGTDE::psql($node, 'postgres', 'DROP TABLESPACE test_tblspace;');

$node->stop;

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare, 0,
	"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files."
);

done_testing();
