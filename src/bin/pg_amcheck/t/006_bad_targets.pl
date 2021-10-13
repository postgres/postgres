
# Copyright (c) 2021, PostgreSQL Global Development Group

# This regression test checks the behavior of pg_amcheck in the presence of
# inappropriate target relations.
#
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 52;

# Establish a primary and standby server, with temporary and unlogged tables.
# The temporary tables should not be checked on either system, as pg_amcheck's
# sessions won't be able to see their contents.  The unlogged tables should not
# be checked on the standby, as they won't have relation forks there.
#
my $node_primary = PostgresNode->new('primary');
$node_primary->init(
	allows_streaming => 1,
	auth_extra       => [ '--create-role', 'repl_role' ]);
$node_primary->start;
$node_primary->safe_psql('postgres', qq(
CREATE EXTENSION amcheck;
CREATE UNLOGGED TABLE unlogged_heap (i INTEGER[], b BOX);
INSERT INTO unlogged_heap (i, b) VALUES (ARRAY[1,2,3]::INTEGER[], '((1,2),(3,4))'::BOX);
CREATE INDEX unlogged_btree ON unlogged_heap USING BTREE (i);
CREATE INDEX unlogged_brin ON unlogged_heap USING BRIN (b);
CREATE INDEX unlogged_gin ON unlogged_heap USING GIN (i);
CREATE INDEX unlogged_gist ON unlogged_heap USING GIST (b);
CREATE INDEX unlogged_hash ON unlogged_heap USING HASH (i);
));
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Hold open a session with temporary tables and indexes defined
#
my $in = '';
my $out = '';
my $timer = IPC::Run::timeout(180);
my $h = $node_primary->background_psql('postgres', \$in, \$out, $timer);
$in = qq(
BEGIN;
CREATE TEMPORARY TABLE heap_temporary (i INTEGER[], b BOX) ON COMMIT PRESERVE ROWS;
INSERT INTO heap_temporary (i, b) VALUES (ARRAY[1,2,3]::INTEGER[], '((1,2),(3,4))'::BOX);
CREATE INDEX btree_temporary ON heap_temporary USING BTREE (i);
CREATE INDEX brin_temporary ON heap_temporary USING BRIN (b);
CREATE INDEX gin_temporary ON heap_temporary USING GIN (i);
CREATE INDEX gist_temporary ON heap_temporary USING GIST (b);
CREATE INDEX hash_temporary ON heap_temporary USING HASH (i);
COMMIT;
BEGIN;
);
$h->pump_nb;

my $node_standby = PostgresNode->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->start;
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('replay'));

# Check that running amcheck functions from SQL against inappropriate targets
# fails sensibly.  This portion of the test arguably belongs in contrib/amcheck
# rather than here, but we have already set up the necessary test environment,
# so check here rather than duplicating the environment there.
#
my ($stdout, $stderr);
for my $test (
	[ $node_standby => "SELECT * FROM verify_heapam('unlogged_heap')",
	  qr/^$/,
	 "checking unlogged table during recovery does not complain" ],

	[ $node_standby => "SELECT * FROM bt_index_check('unlogged_btree')",
	  qr/^$/,
	 "checking unlogged btree index during recovery does not complain" ],

	[ $node_standby => "SELECT * FROM bt_index_check('unlogged_brin')",
	  qr/ERROR:  only B-Tree indexes are supported as targets for verification.*DETAIL:  Relation "unlogged_brin" is not a B-Tree index/s,
	 "checking unlogged brin index during recovery fails appropriately" ],

	[ $node_standby => "SELECT * FROM bt_index_check('unlogged_gin')",
	  qr/ERROR:  only B-Tree indexes are supported as targets for verification.*DETAIL:  Relation "unlogged_gin" is not a B-Tree index/s,
	 "checking unlogged gin index during recovery fails appropriately" ],

	[ $node_standby => "SELECT * FROM bt_index_check('unlogged_gist')",
	  qr/ERROR:  only B-Tree indexes are supported as targets for verification.*DETAIL:  Relation "unlogged_gist" is not a B-Tree index/s,
	 "checking unlogged gist index during recovery fails appropriately" ],

	[ $node_standby => "SELECT * FROM bt_index_check('unlogged_hash')",
	  qr/ERROR:  only B-Tree indexes are supported as targets for verification.*DETAIL:  Relation "unlogged_hash" is not a B-Tree index/s,
	 "checking unlogged hash index during recovery fails appropriately" ],

	)
{
	$test->[0]->psql('postgres', $test->[1],
					 stdout => \$stdout, stderr => \$stderr);
	like ($stderr, $test->[2], $test->[3]);
}

# Verify that --all excludes the temporary relations and handles unlogged
# relations as appropriate, without raising any warnings or exiting with a
# non-zero status.
#
$node_primary->command_checks_all(
	['pg_amcheck', '--all', '-D', 'template1'],
	0,
	[ qr/^$/ ],
	[ qr/^$/ ],
	'pg_amcheck all objects on primary');

$node_standby->command_checks_all(
	['pg_amcheck', '--all', '-D', 'template1'],
	0,
	[ qr/^$/ ],
	[ qr/^$/ ],
	'pg_amcheck all objects on standby');

# Verify that explicitly asking for unlogged relations to be checked does not
# raise any warnings or exit with a non-zero exit status, even when they cannot
# be checked due to recovery being in progress.
#
# These relations will have no relation fork during recovery, so even without
# checking them, we can say (by definition) that they are not corrupt, because
# it is meaningless to declare a non-existent relation fork corrupt.
#
$node_primary->command_checks_all(
	['pg_amcheck', '--relation', '*unlogged*'],
	0,
	[ qr/^$/ ],
	[ qr/^$/ ],
	'pg_amcheck unlogged objects on primary');

$node_standby->command_checks_all(
	['pg_amcheck', '--relation', '*unlogged*'],
	0,
	[ qr/^$/ ],
	[ qr/^$/ ],
	'pg_amcheck unlogged objects on standby');

# Verify that the --heapallindexed check works on both primary and standby.
#
$node_primary->command_checks_all(
	['pg_amcheck', '--all', '-D', 'template1', '--heapallindexed'],
	0,
	[ qr/^$/ ],
	[ qr/^$/ ],
	'pg_amcheck --helpallindexed on primary');

$node_standby->command_checks_all(
	['pg_amcheck', '--all', '-D', 'template1', '--heapallindexed'],
	0,
	[ qr/^$/ ],
	[ qr/^$/ ],
	'pg_amcheck --helpallindexed on standby');

# Verify that the --parent-check and --rootdescend options work on the primary.
#
$node_primary->command_checks_all(
	['pg_amcheck', '--all', '-D', 'template1', '--rootdescend'],
	0,
	[ qr/^$/ ],
	[ qr/^$/ ],
	'pg_amcheck --rootdescend on primary');

$node_primary->command_checks_all(
	['pg_amcheck', '--all', '-D', 'template1', '--parent-check'],
	0,
	[ qr/^$/ ],
	[ qr/^$/ ],
	'pg_amcheck --parent-check on primary');

# Check that the failures on the standby from using --parent-check and
# --rootdescend are the failures we expect
#
$node_standby->command_checks_all(
	['pg_amcheck', '--all', '-D', 'template1', '--rootdescend'],
	2,
	[ qr/ERROR:  cannot acquire lock mode ShareLock on database objects while recovery is in progress/,
	  qr/btree index "postgres\.pg_catalog\./,
	  qr/btree index "postgres\.pg_toast\./,
	],
	[ qr/^$/ ],
	'pg_amcheck --rootdescend on standby');

$node_standby->command_checks_all(
	['pg_amcheck', '--all', '-D', 'template1', '--parent-check'],
	2,
	[ qr/ERROR:  cannot acquire lock mode ShareLock on database objects while recovery is in progress/,
	  qr/btree index "postgres\.pg_catalog\./,
	  qr/btree index "postgres\.pg_toast\./,
	],
	[ qr/^$/ ],
	'pg_amcheck --parent-check on standby');

# Bug #17212
#
# Verify that explicitly asking for another session's temporary relations to be
# checked fails, but only in the sense that nothing matched the parameter.  We
# don't complain that they are uncheckable, only that you gave a --relation
# option and we didn't find anything checkable matching the pattern.
#
$node_primary->command_checks_all(
	['pg_amcheck', '--relation', '*temporary*'],
	1,
	[ qr/^$/ ],
	[ qr/error: no relations to check matching "\*temporary\*"/ ],
	'pg_amcheck temporary objects on primary');

$node_standby->command_checks_all(
	['pg_amcheck', '--relation', '*temporary*'],
	1,
	[ qr/^$/ ],
	[ qr/error: no relations to check matching "\*temporary\*"/ ],
	'pg_amcheck temporary objects on standby');

# Verify that a relation pattern which only matches temporary relations draws
# an error, even when other relation patterns are ok.  This differs from the
# test above in that the set of all relations to check is not empty.
#
$node_primary->command_checks_all(
	['pg_amcheck', '--relation', '*temporary*', '--relation', '*unlogged*'],
	1,
	[ qr/^$/ ],
	[ qr/error: no relations to check matching "\*temporary\*"/ ],
	'pg_amcheck temporary objects on primary');

$node_standby->command_checks_all(
	['pg_amcheck', '--relation', '*temporary*', '--relation', '*unlogged*'],
	1,
	[ qr/^$/ ],
	[ qr/error: no relations to check matching "\*temporary\*"/ ],
	'pg_amcheck temporary objects on standby');
