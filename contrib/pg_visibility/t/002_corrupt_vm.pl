# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Check that pg_check_visible() and pg_check_frozen() report correct TIDs for
# corruption.
use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
# Anything holding a snapshot, including auto-analyze of pg_proc, could stop
# VACUUM from updating the visibility map.
$node->append_conf('postgresql.conf', 'autovacuum=off');
$node->start;

my $blck_size = $node->safe_psql("postgres", "SHOW block_size;");

# Create a sample table with at least 10 pages and then run VACUUM. 10 is
# selected manually as it is big enough to select 5 random tuples from the
# relation.
$node->safe_psql(
	'postgres', qq(
		CREATE EXTENSION pg_visibility;
		CREATE TABLE corruption_test
			WITH (autovacuum_enabled = false) AS
			SELECT
				i,
				repeat('a', 10) AS data
			FROM
				generate_series(1, $blck_size) i;
		VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) corruption_test;
));

# VACUUM is run, it is safe to get the number of pages.
my $npages = $node->safe_psql(
	"postgres",
	"SELECT relpages FROM pg_class
		WHERE relname = 'corruption_test';"
);
ok($npages >= 10, 'table has at least 10 pages');

my $file = $node->safe_psql("postgres",
	"SELECT pg_relation_filepath('corruption_test');");

# Delete the first block to make sure that it will be skipped as it is
# not visible nor frozen.
$node->safe_psql(
	"postgres",
	"DELETE FROM corruption_test
		WHERE (ctid::text::point)[0] = 0;"
);

# Copy visibility map.
$node->stop;
my $vm_file = $node->data_dir . '/' . $file . '_vm';
copy("$vm_file", "${vm_file}_temp");
$node->start;

# Select 5 random tuples that are starting from the second block of the
# relation. The first block is skipped because it is deleted above.
my $tuples = $node->safe_psql(
	"postgres",
	"SELECT ctid FROM (
		SELECT ctid FROM corruption_test
			WHERE (ctid::text::point)[0] != 0
			ORDER BY random() LIMIT 5)
		ORDER BY ctid ASC;"
);

# Do the changes below to use tuples in the query.
# "\n" -> ","
# "(" -> "'("
# ")" -> ")'"
(my $tuples_query = $tuples) =~ s/\n/,/g;
$tuples_query =~ s/\(/\'\(/g;
$tuples_query =~ s/\)/\)\'/g;

$node->safe_psql(
	"postgres",
	"DELETE FROM corruption_test
		WHERE ctid in ($tuples_query);"
);

# Overwrite visibility map with the old one.
$node->stop;
move("${vm_file}_temp", "$vm_file");
$node->start;

my $result = $node->safe_psql(
	"postgres",
	"SELECT DISTINCT t_ctid
		FROM pg_check_visible('corruption_test')
		ORDER BY t_ctid ASC;"
);
is($result, $tuples, 'pg_check_visible must report tuples as corrupted');

$result = $node->safe_psql(
	"postgres",
	"SELECT DISTINCT t_ctid
		FROM pg_check_frozen('corruption_test')
		ORDER BY t_ctid ASC;"
);
is($result, $tuples, 'pg_check_frozen must report tuples as corrupted');

$node->stop;
done_testing();
