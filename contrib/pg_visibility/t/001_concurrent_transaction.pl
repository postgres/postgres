
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Check that a concurrent transaction doesn't cause false negatives in
# pg_check_visible() function
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;


my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->start;

# Setup another database
$node->safe_psql("postgres", "CREATE DATABASE other_database;\n");
my $bsession = $node->background_psql('other_database');

# Run a concurrent transaction
$bsession->query_safe(
	qq[
	BEGIN;
	SELECT txid_current();
]);

# Create a sample table and run vacuum
$node->safe_psql("postgres",
		"CREATE EXTENSION pg_visibility;\n"
	  . "CREATE TABLE vacuum_test AS SELECT 42 i;\n"
	  . "VACUUM (disable_page_skipping) vacuum_test;");

# Run pg_check_visible()
my $result = $node->safe_psql("postgres",
	"SELECT * FROM pg_check_visible('vacuum_test');");

# There should be no false negatives
ok($result eq "", "pg_check_visible() detects no errors");

# Shutdown
$bsession->query_safe("COMMIT;");
$bsession->quit;
$node->stop;

done_testing();
