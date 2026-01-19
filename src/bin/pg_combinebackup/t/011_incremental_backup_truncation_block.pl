# Copyright (c) 2025-2026, PostgreSQL Global Development Group
#
# This test aims to validate that the calculated truncation block never exceeds
# the segment size.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(has_archiving => 1, allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'summarize_wal = on');
$primary->start;

# Backup locations
my $backup_path = $primary->backup_dir;
my $full_backup = "$backup_path/full";

# To avoid using up lots of disk space in the CI/buildfarm environment, this
# test will only find the issue when run with a small RELSEG_SIZE. As of this
# writing, one of the CI runs is configured using --with-segsize-blocks=6, and
# we aim to have this test check for the issue only in that configuration.
my $target_blocks = 6;
my $block_size = $primary->safe_psql('postgres',
	"SELECT current_setting('block_size')::int;");

# We'll have two blocks more than the target number of blocks (one will
# survive the subsequent truncation).
my $target_rows = int($target_blocks + 2);
my $rows_after_truncation = int($target_rows - 1);

# Create a test table. STORAGE PLAIN prevents compression and TOASTing of
# repetitive data, ensuring predictable row sizes.
$primary->safe_psql(
	'postgres', q{
    CREATE TABLE t (
        id int,
        data text STORAGE PLAIN
    );
});

# The tuple size should be enough to prevent two tuples from being on the same
# page. Since the template string has a length of 32 bytes, it's enough to
# repeat it (block_size / (2*32)) times.
$primary->safe_psql(
	'postgres',
	"INSERT INTO t
        SELECT i,
            repeat('0123456789ABCDEF0123456789ABCDEF', ($block_size / (2*32)))
    FROM generate_series(1, $target_rows) i;"
);

# Make sure hint bits are set.
$primary->safe_psql('postgres', 'VACUUM t;');

# Verify that the relation is as large as was desired.
my $t_blocks = $primary->safe_psql('postgres',
	"SELECT pg_relation_size('t') / current_setting('block_size')::int;");
cmp_ok($t_blocks, '>', $target_blocks, 'target block size exceeded');

# Take a full base backup
$primary->backup('full');

# Delete rows at the logical end of the table, creating removable pages.
$primary->safe_psql('postgres',
	"DELETE FROM t WHERE id > ($rows_after_truncation);");

# VACUUM the table. TRUNCATE is enabled by default, and is just mentioned here
# for emphasis.
$primary->safe_psql('postgres', 'VACUUM (TRUNCATE) t;');

# Verify expected length after truncation.
$t_blocks = $primary->safe_psql('postgres',
	"SELECT pg_relation_size('t') / current_setting('block_size')::int;");
is($t_blocks, $rows_after_truncation, 'post-truncation row count as expected');
cmp_ok($t_blocks, '>', $target_blocks,
	'post-truncation block count as expected');

# Take an incremental backup based on the full backup manifest
$primary->backup('incr',
	backup_options => [ '--incremental', "$full_backup/backup_manifest" ]);

# Combine full and incremental backups.  Before the fix, this failed because
# the INCREMENTAL file header contained an incorrect truncation_block value.
my $restored = PostgreSQL::Test::Cluster->new('node2');
$restored->init_from_backup($primary, 'incr', combine_with_prior => ['full']);
$restored->start();

# Check that the restored table contains the correct number of rows
my $restored_count =
  $restored->safe_psql('postgres', "SELECT count(*) FROM t;");
is($restored_count, $rows_after_truncation,
	'Restored backup has correct row count');

$primary->stop;
$restored->stop;

done_testing();
