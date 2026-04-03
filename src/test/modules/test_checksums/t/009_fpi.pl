
# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# Create and start a cluster with one node
my $node = PostgreSQL::Test::Cluster->new('fpi_node');
$node->init(allows_streaming => 1, no_data_checksums => 1);
# max_connections need to be bumped in order to accommodate for pgbench clients
# and log_statement is dialled down since it otherwise will generate enormous
# amounts of logging. Page verification failures are still logged.
$node->append_conf(
	'postgresql.conf',
	qq[
max_connections = 100
log_statement = none
]);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_checksums;');
# Create some content to have un-checksummed data in the cluster
$node->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1, 1000000) AS a;");

# Enable data checksums and wait for the state transition to 'on'
enable_data_checksums($node, wait => 'on');

$node->safe_psql('postgres', 'UPDATE t SET a = a + 1;');

disable_data_checksums($node, wait => 1);

$node->append_conf('postgresql.conf', 'full_page_writes = off');
$node->restart;
test_checksum_state($node, 'off');

$node->safe_psql('postgres', 'UPDATE t SET a = a + 1;');
$node->safe_psql('postgres', 'DELETE FROM t WHERE a < 10000;');

$node->adjust_conf('postgresql.conf', 'full_page_writes', 'on');
$node->restart;
test_checksum_state($node, 'off');

enable_data_checksums($node, wait => 'on');

my $result = $node->safe_psql('postgres', 'SELECT count(*) FROM t;');
is($result, '990003', 'Reading back all data from table t');

$node->stop;
my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile, 0);
unlike(
	$log,
	qr/page verification failed,.+\d$/,
	"no checksum validation errors in server log");

done_testing();
