# Verify that ALTER TABLE optimizes certain operations as expected

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 42;

# Initialize a test cluster
my $node = get_new_node('master');
$node->init();
# Turn message level up to DEBUG1 so that we get the messages we want to see
$node->append_conf('postgresql.conf', 'client_min_messages = DEBUG1');
$node->start;

# Run a SQL command and return psql's stderr (including debug messages)
sub run_sql_command
{
	my $sql = shift;
	my $stderr;

	$node->psql(
		'postgres',
		$sql,
		stderr        => \$stderr,
		on_error_die  => 1,
		on_error_stop => 1);
	return $stderr;
}

# Check whether result of run_sql_command shows that we did a verify pass
sub is_table_verified
{
	my $output = shift;
	return index($output, 'DEBUG:  verifying table') != -1;
}

my $output;

note "test alter table set not null";

run_sql_command(
	'create table atacc1 (test_a int, test_b int);
	 insert into atacc1 values (1, 2);');

$output = run_sql_command('alter table atacc1 alter test_a set not null;');
ok(is_table_verified($output),
	'column test_a without constraint will scan table');

run_sql_command(
	'alter table atacc1 alter test_a drop not null;
	 alter table atacc1 add constraint atacc1_constr_a_valid
	 check(test_a is not null);');

# normal run will verify table data
$output = run_sql_command('alter table atacc1 alter test_a set not null;');
ok(!is_table_verified($output), 'with constraint will not scan table');
ok( $output =~
	  m/existing constraints on column "atacc1.test_a" are sufficient to prove that it does not contain nulls/,
	'test_a proved by constraints');

run_sql_command('alter table atacc1 alter test_a drop not null;');

# we have check only for test_a column, so we need verify table for test_b
$output = run_sql_command(
	'alter table atacc1 alter test_b set not null, alter test_a set not null;'
);
ok(is_table_verified($output), 'table was scanned');
# we may miss debug message for test_a constraint because we need verify table due test_b
ok( !(  $output =~
		m/existing constraints on column "atacc1.test_b" are sufficient to prove that it does not contain nulls/
	),
	'test_b not proved by wrong constraints');
run_sql_command(
	'alter table atacc1 alter test_a drop not null, alter test_b drop not null;'
);

# test with both columns having check constraints
run_sql_command(
	'alter table atacc1 add constraint atacc1_constr_b_valid check(test_b is not null);'
);
$output = run_sql_command(
	'alter table atacc1 alter test_b set not null, alter test_a set not null;'
);
ok(!is_table_verified($output), 'table was not scanned for both columns');
ok( $output =~
	  m/existing constraints on column "atacc1.test_a" are sufficient to prove that it does not contain nulls/,
	'test_a proved by constraints');
ok( $output =~
	  m/existing constraints on column "atacc1.test_b" are sufficient to prove that it does not contain nulls/,
	'test_b proved by constraints');
run_sql_command('drop table atacc1;');

note "test alter table attach partition";

run_sql_command(
	'CREATE TABLE list_parted2 (
	a int,
	b char
	) PARTITION BY LIST (a);
	CREATE TABLE part_3_4 (
	LIKE list_parted2,
	CONSTRAINT check_a CHECK (a IN (3)));');

# need NOT NULL to skip table scan
$output = run_sql_command(
	'ALTER TABLE list_parted2 ATTACH PARTITION part_3_4 FOR VALUES IN (3, 4);'
);
ok(is_table_verified($output), 'table part_3_4 scanned');

run_sql_command(
	'ALTER TABLE list_parted2 DETACH PARTITION part_3_4;
	 ALTER TABLE part_3_4 ALTER a SET NOT NULL;');

$output = run_sql_command(
	'ALTER TABLE list_parted2 ATTACH PARTITION part_3_4 FOR VALUES IN (3, 4);'
);
ok(!is_table_verified($output), 'table part_3_4 not scanned');
ok( $output =~
	  m/partition constraint for table "part_3_4" is implied by existing constraints/,
	'part_3_4 verified by existing constraints');

# test attach default partition
run_sql_command(
	'CREATE TABLE list_parted2_def (
	LIKE list_parted2,
	CONSTRAINT check_a CHECK (a IN (5, 6)));');
$output = run_sql_command(
	'ALTER TABLE list_parted2 ATTACH PARTITION list_parted2_def default;');
ok(!is_table_verified($output), 'table list_parted2_def not scanned');
ok( $output =~
	  m/partition constraint for table "list_parted2_def" is implied by existing constraints/,
	'list_parted2_def verified by existing constraints');

$output = run_sql_command(
	'CREATE TABLE part_55_66 PARTITION OF list_parted2 FOR VALUES IN (55, 66);'
);
ok(!is_table_verified($output), 'table list_parted2_def not scanned');
ok( $output =~
	  m/updated partition constraint for default partition "list_parted2_def" is implied by existing constraints/,
	'updated partition constraint for default partition list_parted2_def');

# test attach another partitioned table
run_sql_command(
	'CREATE TABLE part_5 (
	LIKE list_parted2
	) PARTITION BY LIST (b);
	CREATE TABLE part_5_a PARTITION OF part_5 FOR VALUES IN (\'a\');
	ALTER TABLE part_5 ADD CONSTRAINT check_a CHECK (a IS NOT NULL AND a = 5);'
);
$output = run_sql_command(
	'ALTER TABLE list_parted2 ATTACH PARTITION part_5 FOR VALUES IN (5);');
ok(!($output =~ m/verifying table "part_5"/), 'table part_5 not scanned');
ok($output =~ m/verifying table "list_parted2_def"/,
	'list_parted2_def scanned');
ok( $output =~
	  m/partition constraint for table "part_5" is implied by existing constraints/,
	'part_5 verified by existing constraints');

run_sql_command(
	'ALTER TABLE list_parted2 DETACH PARTITION part_5;
	 ALTER TABLE part_5 DROP CONSTRAINT check_a;');

# scan should again be skipped, even though NOT NULL is now a column property
run_sql_command(
	'ALTER TABLE part_5 ADD CONSTRAINT check_a CHECK (a IN (5)),
	 ALTER a SET NOT NULL;'
);
$output = run_sql_command(
	'ALTER TABLE list_parted2 ATTACH PARTITION part_5 FOR VALUES IN (5);');
ok(!($output =~ m/verifying table "part_5"/), 'table part_5 not scanned');
ok($output =~ m/verifying table "list_parted2_def"/,
	'list_parted2_def scanned');
ok( $output =~
	  m/partition constraint for table "part_5" is implied by existing constraints/,
	'part_5 verified by existing constraints');

# Check the case where attnos of the partitioning columns in the table being
# attached differs from the parent.  It should not affect the constraint-
# checking logic that allows to skip the scan.
run_sql_command(
	'CREATE TABLE part_6 (
	c int,
	LIKE list_parted2,
	CONSTRAINT check_a CHECK (a IS NOT NULL AND a = 6)
	);
	ALTER TABLE part_6 DROP c;');
$output = run_sql_command(
	'ALTER TABLE list_parted2 ATTACH PARTITION part_6 FOR VALUES IN (6);');
ok(!($output =~ m/verifying table "part_6"/), 'table part_6 not scanned');
ok($output =~ m/verifying table "list_parted2_def"/,
	'list_parted2_def scanned');
ok( $output =~
	  m/partition constraint for table "part_6" is implied by existing constraints/,
	'part_6 verified by existing constraints');

# Similar to above, but the table being attached is a partitioned table
# whose partition has still different attnos for the root partitioning
# columns.
run_sql_command(
	'CREATE TABLE part_7 (
	LIKE list_parted2,
	CONSTRAINT check_a CHECK (a IS NOT NULL AND a = 7)
	) PARTITION BY LIST (b);
	CREATE TABLE part_7_a_null (
	c int,
	d int,
	e int,
	LIKE list_parted2,  -- a will have attnum = 4
	CONSTRAINT check_b CHECK (b IS NULL OR b = \'a\'),
	CONSTRAINT check_a CHECK (a IS NOT NULL AND a = 7)
	);
	ALTER TABLE part_7_a_null DROP c, DROP d, DROP e;');

$output = run_sql_command(
	'ALTER TABLE part_7 ATTACH PARTITION part_7_a_null FOR VALUES IN (\'a\', null);'
);
ok(!is_table_verified($output), 'table not scanned');
ok( $output =~
	  m/partition constraint for table "part_7_a_null" is implied by existing constraints/,
	'part_7_a_null verified by existing constraints');
$output = run_sql_command(
	'ALTER TABLE list_parted2 ATTACH PARTITION part_7 FOR VALUES IN (7);');
ok(!is_table_verified($output), 'tables not scanned');
ok( $output =~
	  m/partition constraint for table "part_7" is implied by existing constraints/,
	'part_7 verified by existing constraints');
ok( $output =~
	  m/updated partition constraint for default partition "list_parted2_def" is implied by existing constraints/,
	'updated partition constraint for default partition list_parted2_def');

run_sql_command(
	'CREATE TABLE range_parted (
	a int,
	b int
	) PARTITION BY RANGE (a, b);
	CREATE TABLE range_part1 (
	a int NOT NULL CHECK (a = 1),
	b int NOT NULL);');

$output = run_sql_command(
	'ALTER TABLE range_parted ATTACH PARTITION range_part1 FOR VALUES FROM (1, 1) TO (1, 10);'
);
ok(is_table_verified($output), 'table range_part1 scanned');
ok( !(  $output =~
		m/partition constraint for table "range_part1" is implied by existing constraints/
	),
	'range_part1 not verified by existing constraints');

run_sql_command(
	'CREATE TABLE range_part2 (
	a int NOT NULL CHECK (a = 1),
	b int NOT NULL CHECK (b >= 10 and b < 18)
);');
$output = run_sql_command(
	'ALTER TABLE range_parted ATTACH PARTITION range_part2 FOR VALUES FROM (1, 10) TO (1, 20);'
);
ok(!is_table_verified($output), 'table range_part2 not scanned');
ok( $output =~
	  m/partition constraint for table "range_part2" is implied by existing constraints/,
	'range_part2 verified by existing constraints');

# If a partitioned table being created or an existing table being attached
# as a partition does not have a constraint that would allow validation scan
# to be skipped, but an individual partition does, then the partition's
# validation scan is skipped.
run_sql_command(
	'CREATE TABLE quuux (a int, b text) PARTITION BY LIST (a);
	CREATE TABLE quuux_default PARTITION OF quuux DEFAULT PARTITION BY LIST (b);
	CREATE TABLE quuux_default1 PARTITION OF quuux_default (
	CONSTRAINT check_1 CHECK (a IS NOT NULL AND a = 1)
	) FOR VALUES IN (\'b\');
	CREATE TABLE quuux1 (a int, b text);');

$output = run_sql_command(
	'ALTER TABLE quuux ATTACH PARTITION quuux1 FOR VALUES IN (1);');
ok(is_table_verified($output), 'quuux1 table scanned');
ok( !(  $output =~
		m/partition constraint for table "quuux1" is implied by existing constraints/
	),
	'quuux1 verified by existing constraints');

run_sql_command('CREATE TABLE quuux2 (a int, b text);');
$output = run_sql_command(
	'ALTER TABLE quuux ATTACH PARTITION quuux2 FOR VALUES IN (2);');
ok(!($output =~ m/verifying table "quuux_default1"/),
	'quuux_default1 not scanned');
ok($output =~ m/verifying table "quuux2"/, 'quuux2 scanned');
ok( $output =~
	  m/updated partition constraint for default partition "quuux_default1" is implied by existing constraints/,
	'updated partition constraint for default partition quuux_default1');
run_sql_command('DROP TABLE quuux1, quuux2;');

# should validate for quuux1, but not for quuux2
$output = run_sql_command(
	'CREATE TABLE quuux1 PARTITION OF quuux FOR VALUES IN (1);');
ok(!is_table_verified($output), 'tables not scanned');
ok( !(  $output =~
		m/partition constraint for table "quuux1" is implied by existing constraints/
	),
	'quuux1 verified by existing constraints');
$output = run_sql_command(
	'CREATE TABLE quuux2 PARTITION OF quuux FOR VALUES IN (2);');
ok(!is_table_verified($output), 'tables not scanned');
ok( $output =~
	  m/updated partition constraint for default partition "quuux_default1" is implied by existing constraints/,
	'updated partition constraint for default partition quuux_default1');
run_sql_command('DROP TABLE quuux;');

$node->stop('fast');
