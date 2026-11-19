
# Copyright (c) 2023-2026, PostgreSQL Global Development Group

# Test WAL replay for CREATE DATABASE .. STRATEGY WAL_LOG.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init(allows_streaming => 1);
# Keep the template's pg_class pages in shared buffers between CREATE DATABASE
# and CREATE TABLE.  If CREATE TABLE reloads them after eviction, the checkpoint
# can flush its changes even if CREATE DATABASE failed to mark them permanent.
$node->append_conf('postgresql.conf', 'shared_buffers = 16MB');
$node->start;

$node->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'backup', has_streaming => 1);
$standby->start;

# This checks that any DDLs run on the template database that modify pg_class
# are persisted after creating a database from it using the WAL_LOG strategy,
# as a direct copy of the template database's pg_class is used in this case.
my $db_template = "template1";
my $db_new = "test_db_1";

# Create table.  It should persist on the template database.
$node->safe_psql("postgres",
	"CREATE DATABASE $db_new STRATEGY WAL_LOG TEMPLATE $db_template;");

$node->safe_psql($db_template, "CREATE TABLE tab_db_after_create_1 (a INT);");

# Flush the changes affecting the template database, then replay them.
$node->safe_psql("postgres", "CHECKPOINT;");

$node->stop('immediate');
$node->start;
my $result = $node->safe_psql($db_template,
	"SELECT count(*) FROM pg_class WHERE relname LIKE 'tab_db_%';");
is($result, "1",
	"check that table exists on template after crash, with checkpoint");

# The new database should have no tables.
$result = $node->safe_psql($db_new,
	"SELECT count(*) FROM pg_class WHERE relname LIKE 'tab_db_%';");
is($result, "0",
	"check that there are no tables from template on new database after crash"
);

# Create a table with a populated visibility map in the template database and
# copy it into a second database.
$node->safe_psql($db_template, q{
	CREATE TABLE tab_db_before_create_2 AS
		SELECT generate_series(1, 10000) AS a;
	VACUUM (FREEZE) tab_db_before_create_2;
});
my $db_vm = "test_db_2";
$node->safe_psql("postgres",
	"CREATE DATABASE $db_vm STRATEGY WAL_LOG TEMPLATE $db_template;");
$node->safe_psql($db_vm, "CREATE EXTENSION pg_visibility;");

$node->wait_for_replay_catchup($standby);

my $visibility_map_query = q{
	SELECT blkno, all_visible, all_frozen
	FROM pg_visibility_map('tab_db_before_create_2')
	ORDER BY blkno;
};
my $primary_visibility_map =
  $node->safe_psql($db_vm, $visibility_map_query);
my $standby_visibility_map =
  $standby->safe_psql($db_vm, $visibility_map_query);
my $primary_has_frozen = $node->safe_psql(
	$db_vm,
	q{SELECT bool_or(all_frozen)
	  FROM pg_visibility_map('tab_db_before_create_2');});

is($primary_has_frozen, 't', 'copied table has an all-frozen page');
is($standby_visibility_map, $primary_visibility_map,
	"copied table visibility map matches standby");

done_testing();
