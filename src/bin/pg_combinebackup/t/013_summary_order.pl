# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Check that an incremental backup preserves rows updated on a new
# timeline after VACUUM truncates the table on the previous timeline.
# Summaries are collected from newest to oldest timeline, but must be
# merged in ascending start LSN order to preserve references to blocks
# added after truncation.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $mode = $ENV{PG_TEST_PG_COMBINEBACKUP_MODE} || '--copy';

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', <<EOF);
summarize_wal = on
autovacuum = off
EOF
$primary->start;

# Keep one row per page by storing a half-page payload without compression
# or out-of-line storage.  Four pages fit in one segment even with the
# six-block RELSEG_SIZE used by CI.
my $block_size = $primary->safe_psql('postgres', 'SHOW block_size');
my $pad_size = $block_size / 2;
$primary->safe_psql('postgres', <<EOF);
CREATE TABLE t (id int, pad text STORAGE PLAIN);
INSERT INTO t SELECT g, repeat('x', $pad_size) FROM generate_series(1, 4) g;
EOF

# Set hint bits before taking the full backup.
$primary->safe_psql('postgres', 'VACUUM t');

my $size_sql = "SELECT pg_relation_size('t') / $block_size";
is($primary->safe_psql('postgres', $size_sql),
	'4', 'initial heap has four blocks');

# The backup's checkpoint excludes table creation from the WAL summaries
# needed later, avoiding a zero limit block that would force a full copy.
$primary->backup('full');
my $full_path = $primary->backup_dir . '/full';

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'full', has_streaming => 1);
$standby->start;

# Truncate the table on the old timeline before promoting the standby.
$primary->safe_psql('postgres', 'DELETE FROM t WHERE id > 2');
$primary->safe_psql('postgres', 'VACUUM t');
is($primary->safe_psql('postgres', $size_sql),
	'2', 'VACUUM truncates the heap to two blocks');
$primary->wait_for_replay_catchup($standby);
$primary->stop;
$standby->promote;

# Updating the remaining rows requires new pages, re-extending the heap.
$standby->safe_psql('postgres', "UPDATE t SET pad = pad || 'u'");
is($standby->safe_psql('postgres', $size_sql),
	'4', 'UPDATE re-extends the heap to four blocks');

# Check IDs and values: the deleted rows could reappear without changing
# the row count.
my $contents_sql =
  "SELECT id, pad = (repeat('x', $pad_size) || 'u') FROM t ORDER BY id";
my $expected = "1|t\n2|t";
is($standby->safe_psql('postgres', $contents_sql),
	$expected, 'source contains the two updated rows');

# Store both backups under $primary so init_from_backup() can combine them.
$standby->command_ok(
	[
		'pg_basebackup', '--no-sync',
		'--pgdata' => $primary->backup_dir . '/incr',
		'--checkpoint' => 'fast',
		'--incremental' => "$full_path/backup_manifest",
	],
	'incremental backup after truncation and re-extension');
$standby->stop;

my $restored = PostgreSQL::Test::Cluster->new('restored');
$restored->init_from_backup(
	$primary, 'incr',
	combine_with_prior => ['full'],
	combine_mode => $mode);
$restored->start;
is($restored->safe_psql('postgres', $contents_sql),
	$expected, 'restored rows match the updated source rows');
$restored->stop;

done_testing();
