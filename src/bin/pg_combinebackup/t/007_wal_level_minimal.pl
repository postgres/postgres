# Copyright (c) 2021-2024, PostgreSQL Global Development Group
#
# This test aims to validate that taking an incremental backup fails when
# wal_level has been changed to minimal between the full backup and the
# attempted incremental backup.

use strict;
use warnings FATAL => 'all';
use File::Compare;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Can be changed to test the other modes.
my $mode = $ENV{PG_TEST_PG_COMBINEBACKUP_MODE} || '--copy';

note "testing using mode $mode";

# Set up a new database instance.
my $node1 = PostgreSQL::Test::Cluster->new('node1');
$node1->init(allows_streaming => 1);
$node1->append_conf('postgresql.conf', <<EOM);
summarize_wal = on
wal_keep_size = '1GB'
EOM
$node1->start;

# Create a table and insert a test row into it.
$node1->safe_psql('postgres', <<EOM);
CREATE TABLE mytable (a int, b text);
INSERT INTO mytable VALUES (1, 'finch');
EOM

# Take a full backup.
my $backup1path = $node1->backup_dir . '/backup1';
$node1->command_ok(
	[ 'pg_basebackup', '-D', $backup1path, '--no-sync', '-cfast' ],
	"full backup");

# Switch to wal_level=minimal, which also requires max_wal_senders=0 and
# summarize_wal=off
$node1->safe_psql('postgres', <<EOM);
ALTER SYSTEM SET wal_level = minimal;
ALTER SYSTEM SET max_wal_senders = 0;
ALTER SYSTEM SET summarize_wal = off;
EOM
$node1->restart;

# Insert a second row on the original node.
$node1->safe_psql('postgres', <<EOM);
INSERT INTO mytable VALUES (2, 'gerbil');
EOM

# Revert configuration changes
$node1->safe_psql('postgres', <<EOM);
ALTER SYSTEM RESET wal_level;
ALTER SYSTEM RESET max_wal_senders;
ALTER SYSTEM RESET summarize_wal;
EOM
$node1->restart;

# Now take an incremental backup.
my $backup2path = $node1->backup_dir . '/backup2';
$node1->command_fails_like(
	[
		'pg_basebackup', '-D', $backup2path, '--no-sync', '-cfast',
		'--incremental', $backup1path . '/backup_manifest'
	],
	qr/WAL summaries are required on timeline 1 from.*are incomplete/,
	"incremental backup fails");

# OK, that's all.
done_testing();
