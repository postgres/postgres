# Copyright (c) 2021-2024, PostgreSQL Global Development Group
#
# This test aims to validate that restoring an incremental backup works
# properly even when the reference backup is on a different timeline.

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
$node1->init(has_archiving => 1, allows_streaming => 1);
$node1->append_conf('postgresql.conf', 'summarize_wal = on');
$node1->start;

# Create a table and insert a test row into it.
$node1->safe_psql('postgres', <<EOM);
CREATE TABLE mytable (a int, b text);
INSERT INTO mytable VALUES (1, 'aardvark');
EOM

# Take a full backup.
my $backup1path = $node1->backup_dir . '/backup1';
$node1->command_ok(
	[ 'pg_basebackup', '-D', $backup1path, '--no-sync', '-cfast' ],
	"full backup from node1");

# Insert a second row on the original node.
$node1->safe_psql('postgres', <<EOM);
INSERT INTO mytable VALUES (2, 'beetle');
EOM

# Now take an incremental backup.
my $backup2path = $node1->backup_dir . '/backup2';
$node1->command_ok(
	[
		'pg_basebackup', '-D', $backup2path, '--no-sync', '-cfast',
		'--incremental', $backup1path . '/backup_manifest'
	],
	"incremental backup from node1");

# Restore the incremental backup and use it to create a new node.
my $node2 = PostgreSQL::Test::Cluster->new('node2');
$node2->init_from_backup($node1, 'backup2',
	combine_with_prior => ['backup1']);
$node2->start();

# Insert rows on both nodes.
$node1->safe_psql('postgres', <<EOM);
INSERT INTO mytable VALUES (3, 'crab');
EOM
$node2->safe_psql('postgres', <<EOM);
INSERT INTO mytable VALUES (4, 'dingo');
EOM

# Take another incremental backup, from node2, based on backup2 from node1.
my $backup3path = $node1->backup_dir . '/backup3';
$node2->command_ok(
	[
		'pg_basebackup', '-D', $backup3path, '--no-sync', '-cfast',
		'--incremental', $backup2path . '/backup_manifest'
	],
	"incremental backup from node2");

# Restore the incremental backup and use it to create a new node.
my $node3 = PostgreSQL::Test::Cluster->new('node3');
$node3->init_from_backup(
	$node1, 'backup3',
	combine_with_prior => [ 'backup1', 'backup2' ],
	combine_mode => $mode);
$node3->start();

# Let's insert one more row.
$node3->safe_psql('postgres', <<EOM);
INSERT INTO mytable VALUES (5, 'elephant');
EOM

# Now check that we have the expected rows.
my $result = $node3->safe_psql('postgres', <<EOM);
select string_agg(a::text, ':'), string_agg(b, ':') from mytable;
EOM
is($result, '1:2:4:5|aardvark:beetle:dingo:elephant');

# Let's also verify all the backups.
for my $backup_name (qw(backup1 backup2 backup3))
{
	$node1->command_ok(
		[ 'pg_verifybackup', $node1->backup_dir . '/' . $backup_name ],
		"verify backup $backup_name");
}

# OK, that's all.
done_testing();
