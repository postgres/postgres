# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->append_conf(
	'postgresql.conf', q{
wal_level = 'replica'
max_wal_senders = 4
shared_preload_libraries = 'test_custom_rmgrs'
});
$node->start;

# setup
$node->safe_psql('postgres', 'CREATE EXTENSION test_custom_rmgrs');

# pg_walinspect is required only for verifying test_custom_rmgrs output.
# test_custom_rmgrs doesn't use/depend on it internally.
$node->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');

# make sure checkpoints don't interfere with the test.
my $start_lsn = $node->safe_psql('postgres',
	qq[SELECT lsn FROM pg_create_physical_replication_slot('regress_test_slot1', true, false);]
);

# write and save the WAL record's returned end LSN for verifying it later
my $record_end_lsn = $node->safe_psql('postgres',
	'SELECT * FROM test_custom_rmgrs_insert_wal_record(\'payload123\')');

# ensure the WAL is written and flushed to disk
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');

my $end_lsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');

# check if our custom WAL resource manager has successfully registered with the server
my $row_count = $node->safe_psql(
	'postgres',
	qq[SELECT count(*) FROM pg_get_wal_resource_managers()
		WHERE rm_name = 'test_custom_rmgrs';]);
is($row_count, '1',
	'custom WAL resource manager has successfully registered with the server'
);

# check if our custom WAL resource manager has successfully written a WAL record
my $expected =
  qq($record_end_lsn|test_custom_rmgrs|TEST_CUSTOM_RMGRS_MESSAGE|0|payload (10 bytes): payload123);
my $result = $node->safe_psql(
	'postgres',
	qq[SELECT end_lsn, resource_manager, record_type, fpi_length, description FROM pg_get_wal_records_info('$start_lsn', '$end_lsn')
		WHERE resource_manager = 'test_custom_rmgrs';]);
is($result, $expected,
	'custom WAL resource manager has successfully written a WAL record');

$node->stop;
done_testing();
