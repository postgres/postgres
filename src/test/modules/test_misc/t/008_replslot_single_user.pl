# Copyright (c) 2025, PostgreSQL Global Development Group

# Test manipulations of replication slots with the single-user mode.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Skip the tests on Windows, as single-user mode would fail on permission
# failure with privileged accounts.
if ($windows_os)
{
	plan skip_all => 'this test is not supported by this platform';
}

# Run set of queries in single-user mode.
sub test_single_mode
{
	my ($node, $queries, $testname) = @_;

	my $result = run_log(
		[
			'postgres', '--single', '-F',
			'-c' => 'exit_on_error=true',
			'-D' => $node->data_dir,
			'postgres'
		],
		'<' => \$queries);

	ok($result, $testname);
}

my $slot_logical = 'slot_logical';
my $slot_physical = 'slot_physical';

# Initialize a node
my $node = PostgreSQL::Test::Cluster->new('node');
$node->init(allows_streaming => "logical");
$node->start;

# Define initial table
$node->safe_psql('postgres', "CREATE TABLE foo (id int)");

$node->stop;

test_single_mode(
	$node,
	"SELECT pg_create_logical_replication_slot('$slot_logical', 'test_decoding')",
	"logical slot creation");
test_single_mode(
	$node,
	"SELECT pg_create_physical_replication_slot('$slot_physical', true)",
	"physical slot creation");
test_single_mode(
	$node,
	"SELECT pg_create_physical_replication_slot('slot_tmp', true, true)",
	"temporary physical slot creation");

test_single_mode(
	$node, qq(
INSERT INTO foo VALUES (1);
SELECT pg_logical_slot_get_changes('$slot_logical', NULL, NULL);
),
	"logical decoding");

test_single_mode(
	$node,
	"SELECT pg_replication_slot_advance('$slot_logical', pg_current_wal_lsn())",
	"logical slot advance");
test_single_mode(
	$node,
	"SELECT pg_replication_slot_advance('$slot_physical', pg_current_wal_lsn())",
	"physical slot advance");

test_single_mode(
	$node,
	"SELECT pg_copy_logical_replication_slot('$slot_logical', 'slot_log_copy')",
	"logical slot copy");
test_single_mode(
	$node,
	"SELECT pg_copy_physical_replication_slot('$slot_physical', 'slot_phy_copy')",
	"physical slot copy");

test_single_mode(
	$node,
	"SELECT pg_drop_replication_slot('$slot_logical')",
	"logical slot drop");
test_single_mode(
	$node,
	"SELECT pg_drop_replication_slot('$slot_physical')",
	"physical slot drop");

done_testing();
