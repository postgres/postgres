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

if (index(lc($PG_VERSION_STRING), lc("percona")) == -1)
{
    plan skip_all => "pg_tde test case only for PPG server package install with extensions.";
}

# CREATE new PostgreSQL node and do initdb
my $node = PGTDE->pgtde_init_pg();
my $pgdata = $node->data_dir;

copy("$pgdata/postgresql.conf", "$pgdata/postgresql.conf.bak");

# UPDATE postgresql.conf to include/load pg_stat_monitor library
open my $conf, '>>', "$pgdata/postgresql.conf";
print $conf "shared_preload_libraries = 'pg_tde, pg_stat_monitor, pgaudit, set_user, pg_repack'\n";
print $conf "pg_stat_monitor.pgsm_bucket_time = 360000\n";
print $conf "pg_stat_monitor.pgsm_normalized_query = 'yes'\n";
close $conf;

open my $conf2, '>>', "/tmp/datafile-location";
print $conf2 "/tmp/keyring_data_file\n";
close $conf2;

# Start server
my $rt_value = $node->start;
ok($rt_value == 1, "Start Server");

# Create PGSM extension
my ($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION pg_stat_monitor;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE PGSM EXTENSION");
PGTDE::append_to_debug_file($stdout);

($cmdret, $stdout, $stderr) = $node->psql('postgres', 'SELECT pg_stat_monitor_reset();', extra_params => ['-a', '-Pformat=aligned','-Ptuples_only=off']);
ok($cmdret == 0, "Reset PGSM EXTENSION");
PGTDE::append_to_debug_file($stdout);

# Create pg_tde extension
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE PGTDE EXTENSION");
PGTDE::append_to_file($stdout);

# Create Other extensions
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS pgaudit;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE pgaudit EXTENSION");
PGTDE::append_to_debug_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS set_user;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE set_user EXTENSION");
PGTDE::append_to_debug_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS pg_repack;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE pg_repack EXTENSION");
PGTDE::append_to_debug_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SET pgaudit.log = 'none'; CREATE EXTENSION IF NOT EXISTS postgis; SET pgaudit.log = 'all';", extra_params => ['-a']);
ok($cmdret == 0, "CREATE postgis EXTENSION");
PGTDE::append_to_debug_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS postgis_raster;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE postgis_raster EXTENSION");
PGTDE::append_to_debug_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS postgis_sfcgal;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE postgis_sfcgal EXTENSION");
PGTDE::append_to_debug_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS fuzzystrmatch;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE fuzzystrmatch EXTENSION");
PGTDE::append_to_debug_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS address_standardizer;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE address_standardizer EXTENSION");
PGTDE::append_to_debug_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS address_standardizer_data_us;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE address_standardizer_data_us EXTENSION");
PGTDE::append_to_debug_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS postgis_tiger_geocoder;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE postgis_tiger_geocoder EXTENSION");
PGTDE::append_to_debug_file($stdout);

$rt_value = $node->psql('postgres', "SELECT pg_tde_add_key_provider_file('file-provider', json_object( 'type' VALUE 'file', 'path' VALUE '/tmp/datafile-location' ));", extra_params => ['-a']);
$rt_value = $node->psql('postgres', "SELECT pg_tde_set_principal_key('test-db-principal-key','file-provider');", extra_params => ['-a']);

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc1(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING pg_tde_basic;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc1 (k) VALUES (5),(6);', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc1 ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc1 ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'DROP TABLE test_enc1;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Print PGSM settings 
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT name, setting, unit, context, vartype, source, min_val, max_val, enumvals, boot_val, reset_val, pending_restart FROM pg_settings WHERE name='pg_stat_monitor.pgsm_query_shared_buffer';", extra_params => ['-a', '-Pformat=aligned','-Ptuples_only=off']);
ok($cmdret == 0, "Print PGTDE EXTENSION Settings");
PGTDE::append_to_debug_file($stdout);

# Create example database and run pgbench init
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE database example;', extra_params => ['-a']);
print "cmdret $cmdret\n";
ok($cmdret == 0, "CREATE Database example");
PGTDE::append_to_debug_file($stdout);

my $port = $node->port;
print "port $port \n";

my $out = system ("pgbench -i -s 20 -p $port example");
print " out: $out \n";
ok($cmdret == 0, "Perform pgbench init");

$out = system ("pgbench -c 10 -j 2 -t 5000 -p $port example");
print " out: $out \n";
ok($cmdret == 0, "Run pgbench");

($cmdret, $stdout, $stderr) = $node->psql('postgres', 'SELECT datname, substr(query,0,150) AS query, SUM(calls) AS calls FROM pg_stat_monitor GROUP BY datname, query ORDER BY datname, query, calls;', extra_params => ['-a', '-Pformat=aligned','-Ptuples_only=off']);
ok($cmdret == 0, "SELECT XXX FROM pg_stat_monitor");
PGTDE::append_to_debug_file($stdout);

# DROP EXTENSION
$stdout = $node->safe_psql('postgres', 'DROP EXTENSION pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "DROP PGTDE EXTENSION");
PGTDE::append_to_file($stdout);

# DROP EXTENSION
$stdout = $node->safe_psql('postgres', 'DROP EXTENSION pg_stat_monitor;', extra_params => ['-a']);
ok($cmdret == 0, "DROP PGTDE EXTENSION");
PGTDE::append_to_debug_file($stdout);

# Stop the server
$node->stop();

# compare the expected and out file
my $compare = PGTDE->compare_results();

# Test/check if expected and result/out file match. If Yes, test passes.
is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

# Done testing for this testcase file.
done_testing();
