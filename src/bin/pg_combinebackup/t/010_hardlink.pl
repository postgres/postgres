# Copyright (c) 2025, PostgreSQL Global Development Group
#
# This test aims to validate that hard links are created as expected in the
# output directory, when running pg_combinebackup with --link mode.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Set up a new database instance.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(has_archiving => 1, allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'summarize_wal = on');
# We disable autovacuum to prevent "something else" to modify our test tables.
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;

# Create a couple of tables (~264KB each).
# Note: Cirrus CI runs some tests with a very small segment size, so, in that
# environment, a single table of 264KB would have both a segment with a link
# count of 1 and also one with a link count of 2. But in a normal installation,
# segment size is 1GB.  Therefore, we use 2 different tables here: for test_1,
# all segments (or the only one) will have two hard links; for test_2, the
# last segment (or the only one) will have 1 hard link, and any others will
# have 2.
my $query = <<'EOM';
CREATE TABLE test_%s AS
    SELECT x.id::bigint,
           repeat('a', 1600) AS value
    FROM generate_series(1, 100) AS x(id);
EOM

$primary->safe_psql('postgres', sprintf($query, '1'));
$primary->safe_psql('postgres', sprintf($query, '2'));

# Fetch information about the data files.
$query = <<'EOM';
SELECT pg_relation_filepath(oid)
FROM pg_class
WHERE relname = 'test_%s';
EOM

my $test_1_path = $primary->safe_psql('postgres', sprintf($query, '1'));
note "test_1 path is $test_1_path";

my $test_2_path = $primary->safe_psql('postgres', sprintf($query, '2'));
note "test_2 path is $test_2_path";

# Take a full backup.
my $backup1path = $primary->backup_dir . '/backup1';
$primary->command_ok(
	[
		'pg_basebackup',
		'--pgdata' => $backup1path,
		'--no-sync',
		'--checkpoint' => 'fast',
        '--wal-method' => 'none'
	],
	"full backup");

# Perform an insert that touches a page of the last segment of the data file of
# table test_2.
$primary->safe_psql('postgres', <<EOM);
INSERT INTO test_2 (id, value) VALUES (101, repeat('a', 1600));
EOM

# Take an incremental backup.
my $backup2path = $primary->backup_dir . '/backup2';
$primary->command_ok(
	[
		'pg_basebackup',
		'--pgdata' => $backup2path,
		'--no-sync',
		'--checkpoint' => 'fast',
        '--wal-method' => 'none',
		'--incremental' => $backup1path . '/backup_manifest'
	],
	"incremental backup");

# Restore the incremental backup and use it to create a new node.
my $restore = PostgreSQL::Test::Cluster->new('restore');
$restore->init_from_backup(
	$primary, 'backup2',
	combine_with_prior => ['backup1'],
	combine_mode => '--link');

# Ensure files have the expected count of hard links. We expect all data files
# from test_1 to contain 2 hard links, because they were not touched between the
# full and incremental backups, and the last data file of table test_2 to
# contain a single hard link because of changes in its last page.
my $test_1_full_path = join('/', $restore->data_dir, $test_1_path);
check_data_file($test_1_full_path, 2);

my $test_2_full_path = join('/', $restore->data_dir, $test_2_path);
check_data_file($test_2_full_path, 1);

# OK, that's all.
done_testing();


# Given the path to the first segment of a data file, inspect its parent
# directory to find all the segments of that data file, and make sure all the
# segments contain 2 hard links. The last one must have the given number of hard
# links.
#
# Parameters:
# * data_file: path to the first segment of a data file, as per the output of
#              pg_relation_filepath.
# * last_segment_nlinks: the number of hard links expected in the last segment
#                        of the given data file.
sub check_data_file
{
    my ($data_file, $last_segment_nlinks) = @_;

    my @data_file_segments = ($data_file);

    # Start checking for additional segments
    my $segment_number = 1;

    while (1)
    {
        my $next_segment = $data_file . '.' . $segment_number;

        # If the file exists and is a regular file, add it to the list
        if (-f $next_segment)
        {
            push @data_file_segments, $next_segment;
            $segment_number++;
        }
        # Stop the loop if the file doesn't exist
        else
        {
            last;
        }
    }

    # All segments of the given data file should contain 2 hard links, except
    # for the last one, which should match the given number of links.
    my $last_segment = pop @data_file_segments;

    for my $segment (@data_file_segments)
    {
        # Get the file's stat information of each segment
        my $nlink_count = get_hard_link_count($segment);
        ok($nlink_count == 2, "File '$segment' has 2 hard links");
    }

    # Get the file's stat information of the last segment
    my $nlink_count = get_hard_link_count($last_segment);
    ok($nlink_count == $last_segment_nlinks,
       "File '$last_segment' has $last_segment_nlinks hard link(s)");
}


# Subroutine to get hard link count of a given file.
# Receives the path to a file, and returns the number of hard links of
# that file.
sub get_hard_link_count
{
    my ($file) = @_;

    # Get file stats
    my @stats = stat($file);
    my $nlink = $stats[3];  # Number of hard links

    return $nlink;
}
