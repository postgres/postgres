#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

unlink('/tmp/replication.per');

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', q{
checkpoint_timeout = 1h
shared_preload_libraries = 'pg_tde'
});
$primary->start;

$primary->backup('backup');
my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->init_from_backup($primary, 'backup', has_streaming => 1);
$replica->set_standby_mode();
$replica->start;

PGTDE::append_to_result_file("-- At primary");

PGTDE::psql($primary, 'postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;');
PGTDE::psql($primary, 'postgres',
	"SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/replication.per');"
);
PGTDE::psql($primary, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('test-key', 'file-vault');"
);

PGTDE::psql($primary, 'postgres',
	"CREATE TABLE test_enc (x int PRIMARY KEY) USING tde_heap;");
PGTDE::psql($primary, 'postgres',
	"INSERT INTO test_enc (x) VALUES (1), (2);");

PGTDE::psql($primary, 'postgres',
	"CREATE TABLE test_plain (x int PRIMARY KEY) USING heap;");
PGTDE::psql($primary, 'postgres',
	"INSERT INTO test_plain (x) VALUES (3), (4);");

$primary->wait_for_catchup('replica');

PGTDE::append_to_result_file("-- At replica");

PGTDE::psql($replica, 'postgres', "SELECT pg_tde_is_encrypted('test_enc');");
PGTDE::psql($replica, 'postgres',
	"SELECT pg_tde_is_encrypted('test_enc_pkey');");
PGTDE::psql($replica, 'postgres', "SELECT * FROM test_enc ORDER BY x;");

PGTDE::psql($replica, 'postgres',
	"SELECT pg_tde_is_encrypted('test_plain');");
PGTDE::psql($replica, 'postgres',
	"SELECT pg_tde_is_encrypted('test_plain_pkey');");
PGTDE::psql($replica, 'postgres', "SELECT * FROM test_plain ORDER BY x;");

PGTDE::append_to_result_file("-- check primary crash with WAL encryption");
PGTDE::psql($primary, 'postgres',
	"SELECT pg_tde_add_global_key_provider_file('file-vault', '/tmp/unlogged_tables.per');"
);
PGTDE::psql($primary, 'postgres',
	"SELECT pg_tde_set_server_key_using_global_key_provider('test-global-key', 'file-vault');"
);

PGTDE::psql($primary, 'postgres',
	"CREATE TABLE test_enc2 (x int PRIMARY KEY) USING tde_heap;");
PGTDE::psql($primary, 'postgres',
	"INSERT INTO test_enc2 (x) VALUES (1), (2);");

PGTDE::psql($primary, 'postgres',
	"ALTER SYSTEM SET pg_tde.wal_encrypt = 'on';");
PGTDE::kill9_until_dead($primary);

PGTDE::append_to_result_file("-- primary start");
$primary->start;
$primary->wait_for_catchup('replica');

PGTDE::psql($replica, 'postgres', "SELECT * FROM test_enc2 ORDER BY x;");

$replica->stop;
$primary->stop;

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare, 0,
	"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files."
);

done_testing();
