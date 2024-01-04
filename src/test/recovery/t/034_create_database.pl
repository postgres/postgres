
# Copyright (c) 2023-2024, PostgreSQL Global Development Group

# Test WAL replay for CREATE DATABASE .. STRATEGY WAL_LOG.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

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

done_testing();
