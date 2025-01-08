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

my $PG_VERSION_STRING = `pg_config --version`;

if (index(lc($PG_VERSION_STRING), lc("Percona Server")) == -1)
{
    plan skip_all => "pg_tde test case only for Percona Server for PostgreSQL";
}

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

# CREATE EXTENSION and change out file permissions
my ($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE PGTDE EXTENSION");
PGTDE::append_to_file($stdout);


$rt_value = $node->psql('postgres', 'CREATE TABLE test_enc(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING tde_heap;', extra_params => ['-a']);
ok($rt_value == 3, "Failing query");


# Restart the server
PGTDE::append_to_file("-- server restart");
$node->stop();

$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

$rt_value = $node->psql('postgres', "SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');", extra_params => ['-a']);
$rt_value = $node->psql('postgres', "SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');", extra_params => ['-a']);



######################### test_enc1 (simple create table w tde_heap)


$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc1(id SERIAL,k VARCHAR(32),PRIMARY KEY (id)) USING tde_heap;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc1 (k) VALUES (\'foobar\'),(\'barfoo\');', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc1 ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

######################### test_enc2 (create heap + alter to tde_heap)

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc2(id SERIAL,k VARCHAR(32),PRIMARY KEY (id));', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc2 (k) VALUES (\'foobar\'),(\'barfoo\');', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'ALTER TABLE test_enc2 SET ACCESS METHOD tde_heap;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc2 ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

######################### test_enc3 (default_table_access_method)

$stdout = $node->safe_psql('postgres', 'SET default_table_access_method = "tde_heap"; CREATE TABLE test_enc3(id SERIAL,k VARCHAR(32),PRIMARY KEY (id));', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc3 (k) VALUES (\'foobar\'),(\'barfoo\');', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc3 ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

######################### test_enc4 (create heap + alter default)

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc4(id SERIAL,k VARCHAR(32),PRIMARY KEY (id)) USING heap;', extra_params => ['-a']);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc4 (k) VALUES (\'foobar\'),(\'barfoo\');', extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SET default_table_access_method = "tde_heap"; ALTER TABLE test_enc4 SET ACCESS METHOD DEFAULT;', extra_params => ['-a']);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc4 ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);


######################### test_enc5 (create tde_heap + truncate)

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc5(id SERIAL,k VARCHAR(32),PRIMARY KEY (id)) USING tde_heap;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc5 (k) VALUES (\'foobar\'),(\'barfoo\');', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'CHECKPOINT;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'TRUNCATE test_enc5;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc5 (k) VALUES (\'foobar\'),(\'barfoo\');', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc5 ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

sub verify_table
{
    PGTDE::append_to_file('###########################');

    my ($table) = @_;

    my $tablefile = $node->safe_psql('postgres', 'SHOW data_directory;');
    $tablefile .= '/';
    $tablefile .= $node->safe_psql('postgres', 'SELECT pg_relation_filepath(\''.$table.'\');');

    $stdout = $node->safe_psql('postgres', 'SELECT * FROM ' . $table . ' ORDER BY id ASC;', extra_params => ['-a']);
    PGTDE::append_to_file($stdout);

    my $strings = 'TABLEFILE FOR ' . $table . ' FOUND: ';
    $strings .= `(ls  $tablefile >/dev/null && echo -n yes) || echo -n no`;
    PGTDE::append_to_file($strings);

    $strings = 'CONTAINS FOO (should be empty): ';
    $strings .= `strings $tablefile | grep foo`;
    PGTDE::append_to_file($strings);
}

verify_table('test_enc1');
verify_table('test_enc2');
verify_table('test_enc3');
verify_table('test_enc4');
verify_table('test_enc5');

# Verify that we can't see the data in the file
my $tablefile2 = $node->safe_psql('postgres', 'SHOW data_directory;');
$tablefile2 .= '/';
$tablefile2 .= $node->safe_psql('postgres', 'SELECT pg_relation_filepath(\'test_enc2\');');

my  $strings = 'TABLEFILE2 FOUND: ';
$strings .= `(ls  $tablefile2 >/dev/null && echo yes) || echo no`;
PGTDE::append_to_file($strings);

$strings = 'CONTAINS FOO (should be empty): ';
$strings .= `strings $tablefile2 | grep foo`;
PGTDE::append_to_file($strings);




# Verify that we can't see the data in the file
my $tablefile3 = $node->safe_psql('postgres', 'SHOW data_directory;');
$tablefile3 .= '/';
$tablefile3 .= $node->safe_psql('postgres', 'SELECT pg_relation_filepath(\'test_enc3\');');

$strings = 'TABLEFILE3 FOUND: ';
$strings .= `(ls  $tablefile3 >/dev/null && echo yes) || echo no`;
PGTDE::append_to_file($strings);

$strings = 'CONTAINS FOO (should be empty): ';
$strings .= `strings $tablefile3 | grep foo`;
PGTDE::append_to_file($strings);




# Verify that we can't see the data in the file
my $tablefile4 = $node->safe_psql('postgres', 'SHOW data_directory;');
$tablefile4 .= '/';
$tablefile4 .= $node->safe_psql('postgres', 'SELECT pg_relation_filepath(\'test_enc4\');');

$strings = 'TABLEFILE4 FOUND: ';
$strings .= `(ls  $tablefile4 >/dev/null && echo yes) || echo no`;
PGTDE::append_to_file($strings);

$strings = 'CONTAINS FOO (should be empty): ';
$strings .= `strings $tablefile4 | grep foo`;
PGTDE::append_to_file($strings);



$stdout = $node->safe_psql('postgres', 'DROP TABLE test_enc1;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'DROP TABLE test_enc2;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'DROP TABLE test_enc3;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'DROP TABLE test_enc4;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'DROP TABLE test_enc5;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# DROP EXTENSION
$stdout = $node->safe_psql('postgres', 'DROP EXTENSION pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "DROP PGTDE EXTENSION");
PGTDE::append_to_file($stdout);
# Stop the server
$node->stop();

# compare the expected and out file
my $compare = PGTDE->compare_results();

# Test/check if expected and result/out file match. If Yes, test passes.
is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

# Done testing for this testcase file.
done_testing();
