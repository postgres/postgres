
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test suite for testing enabling data checksums in an online cluster
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# Initialize node with checksums disabled.
my $node = PostgreSQL::Test::Cluster->new('basic_node');
$node->init(no_data_checksums => 1);
$node->start;

# Create some content to have un-checksummed data in the cluster
$node->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");

# Ensure that checksums are turned off
test_checksum_state($node, 'off');

# Enable data checksums and wait for the state transition to 'on'
enable_data_checksums($node, wait => 'on');

# Run a dummy query just to make sure we can read back data
my $result =
  $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1 ");
is($result, '9999', 'ensure checksummed pages can be read back');

# Enable data checksums again which should be a no-op so we explicitly don't
# wait for any state transition as none should happen here.
enable_data_checksums($node);
test_checksum_state($node, 'on');
# ..and make sure we can still read/write data
$node->safe_psql('postgres', "UPDATE t SET a = a + 1;");
$result = $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
is($result, '10000', 'ensure checksummed pages can be read back');

# Disable checksums again and wait for the state transition
disable_data_checksums($node, wait => 1);

# Test reading data again
$result = $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
is($result, '10000', 'ensure previously checksummed pages can be read back');

# Re-enable checksums and make sure that the underlying data has changed to
# ensure that checksums will be different.
$node->safe_psql('postgres', "UPDATE t SET a = a + 1;");
enable_data_checksums($node, wait => 'on');

# Run a dummy query just to make sure we can read back the data
$result = $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
is($result, '10000', 'ensure checksummed pages can be read back');

$node->stop;
done_testing();
