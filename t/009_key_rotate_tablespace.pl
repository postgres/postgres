#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use File::Compare;
use File::Copy;
use Test::More;
use lib 't';
use pgtde;

# Get file name and CREATE out file name and dirs WHERE requried
PGTDE::setup_files_dir(basename($0));

my ($cmdret, $stdout);

# CREATE new PostgreSQL node and do initdb
my $node = PGTDE->pgtde_init_pg();
my $pgdata = $node->data_dir;

# UPDATE postgresql.conf to include/load pg_tde library
open my $conf, '>>', "$pgdata/postgresql.conf";
print $conf "shared_preload_libraries = 'pg_tde'\n";
close $conf;

# Start server
my $rt_value = $node->start;
ok($rt_value == 1, "Start Server");

$node->safe_psql('postgres',
	q{
SET allow_in_place_tablespaces = true;
CREATE TABLESPACE test_tblspace LOCATION '';
CREATE DATABASE tbc TABLESPACE = test_tblspace;
});

$stdout = $node->safe_psql('tbc',
	q{
CREATE EXTENSION pg_tde;
SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');

CREATE TABLE country_table (
     country_id        serial primary key,
     country_name    text unique not null,
     continent        text not null
) USING tde_heap_basic;

INSERT INTO country_table (country_name, continent)
     VALUES ('Japan', 'Asia'),
            ('UK', 'Europe'),
            ('USA', 'North America');

SELECT * FROM country_table;

}, extra_params => ['-a']);
PGTDE::append_to_file($stdout);


$cmdret = $node->psql('tbc', "SELECT pg_tde_rotate_principal_key('new-k', 'file-vault');", extra_params => ['-a']);
ok($cmdret == 0, "ROTATE KEY");
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$node->stop();

$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

$stdout = $node->safe_psql('tbc', 'SELECT * FROM country_table;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);


# DROP EXTENSION
$stdout = $node->safe_psql('tbc', 'DROP EXTENSION pg_tde CASCADE;', extra_params => ['-a']);
ok($cmdret == 0, "DROP PGTDE EXTENSION");
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', q{
DROP DATABASE tbc;
DROP TABLESPACE test_tblspace;
}, extra_params => ['-a']);
ok($cmdret == 0, "DROP DATABSE");
PGTDE::append_to_file($stdout);
# Stop the server
$node->stop();

# compare the expected and out file
my $compare = PGTDE->compare_results();

# Test/check if expected and result/out file match. If Yes, test passes.
is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

# Done testing for this testcase file.
done_testing();
