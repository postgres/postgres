# Copyright (c) 2025-2026, PostgreSQL Global Development Group
#
# Check how the copy of WAL segments is handled from the source to
# the target server.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;
use File::stat qw(stat);

use FindBin;
use lib $FindBin::RealBin;
use RewindTest;

RewindTest::setup_cluster();
RewindTest::start_primary();
RewindTest::create_standby();

# Advance WAL on primary
RewindTest::primary_psql("CREATE TABLE t(a int)");
RewindTest::primary_psql("INSERT INTO t VALUES(0)");

# Segment that is not copied from the source to the target, being
# generated before the servers have diverged.
my $wal_seg_skipped = $node_primary->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');

RewindTest::primary_psql("SELECT pg_switch_wal()");

# Follow-up segment, that will include corrupted contents, and will be
# copied from the source to the target even if generated before the point
# of divergence.
RewindTest::primary_psql("INSERT INTO t VALUES(0)");
my $corrupt_wal_seg = $node_primary->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');
RewindTest::primary_psql("SELECT pg_switch_wal()");

RewindTest::primary_psql("CHECKPOINT");
RewindTest::promote_standby;

# New segment on a new timeline, expected to be copied.
my $new_timeline_wal_seg = $node_standby->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');

# Corrupt a WAL segment on target that has been generated before the
# divergence point.  We will check that it is copied from the source.
my $corrupt_wal_seg_in_target_path =
  $node_primary->data_dir . '/pg_wal/' . $corrupt_wal_seg;
open my $fh, ">>", $corrupt_wal_seg_in_target_path
  or die "could not open $corrupt_wal_seg_in_target_path";

print $fh 'a';
close $fh;

my $corrupt_wal_seg_stat_before_rewind =
  stat($corrupt_wal_seg_in_target_path);
ok(defined($corrupt_wal_seg_stat_before_rewind),
	"segment $corrupt_wal_seg exists in target before rewind");

# Verify that the WAL segment on the new timeline does not exist in target
# before the rewind.
my $new_timeline_wal_seg_path =
  $node_primary->data_dir . '/pg_wal/' . $new_timeline_wal_seg;
my $new_timeline_wal_seg_stat = stat($new_timeline_wal_seg_path);
ok(!defined($new_timeline_wal_seg_stat),
	"segment $new_timeline_wal_seg does not exist in target before rewind");

$node_standby->stop();
$node_primary->stop();

# Cross-check how WAL segments are handled:
# - The "corrupted" segment generated before the point of divergence is
#   copied.
# - The "clean" segment generated before the point of divergence is skipped.
# - The segment of the new timeline is copied.
command_checks_all(
	[
		'pg_rewind', '--debug',
		'--source-pgdata' => $node_standby->data_dir,
		'--target-pgdata' => $node_primary->data_dir,
		'--no-sync',
	],
	0,
	[qr//],
	[
		qr/pg_wal\/$wal_seg_skipped \(NONE\)/,
		qr/pg_wal\/$corrupt_wal_seg \(COPY\)/,
		qr/pg_wal\/$new_timeline_wal_seg \(COPY\)/,
	],
	'run pg_rewind');

# Verify that the first WAL segment of the new timeline now exists in
# target.
$new_timeline_wal_seg_stat = stat($new_timeline_wal_seg_path);
ok(defined($new_timeline_wal_seg_stat),
	"new timeline segment $new_timeline_wal_seg exists in target after rewind"
);

# Validate that the WAL segment with the same file name as the
# corrupted WAL segment in target has been copied from source
# where it was still intact.
my $corrupt_wal_seg_in_source_path =
  $node_standby->data_dir . '/pg_wal/' . $corrupt_wal_seg;
my $corrupt_wal_seg_source_stat = stat($corrupt_wal_seg_in_source_path);
ok(defined($corrupt_wal_seg_source_stat),
	"corrupted $corrupt_wal_seg exists in source after rewind");

my $corrupt_wal_seg_stat_after_rewind = stat($corrupt_wal_seg_in_target_path);
ok(defined($corrupt_wal_seg_stat_after_rewind),
	"corrupted $corrupt_wal_seg exists in target after rewind");
isnt(
	$corrupt_wal_seg_stat_before_rewind->size,
	$corrupt_wal_seg_source_stat->size,
	"different size of corrupted $corrupt_wal_seg in source vs target before rewind"
);
is( $corrupt_wal_seg_stat_after_rewind->size,
	$corrupt_wal_seg_source_stat->size,
	"same size of corrupted $corrupt_wal_seg in source and target after rewind"
);

done_testing();
