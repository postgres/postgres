
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Tests that unlogged tables are properly reinitialized after a crash.
#
# The behavior should be the same when restoring from a backup, but
# that is not tested here.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->start;
my $pgdata = $node->data_dir;

# Create an unlogged table and an unlogged sequence to test that forks
# other than init are not copied.
$node->safe_psql('postgres', 'CREATE UNLOGGED TABLE base_unlogged (id int)');
$node->safe_psql('postgres', 'CREATE UNLOGGED SEQUENCE seq_unlogged');

my $baseUnloggedPath = $node->safe_psql('postgres',
	q{select pg_relation_filepath('base_unlogged')});
my $seqUnloggedPath = $node->safe_psql('postgres',
	q{select pg_relation_filepath('seq_unlogged')});

# Test that main and init forks exist.
ok(-f "$pgdata/${baseUnloggedPath}_init", 'table init fork exists');
ok(-f "$pgdata/$baseUnloggedPath",        'table main fork exists');
ok(-f "$pgdata/${seqUnloggedPath}_init",  'sequence init fork exists');
ok(-f "$pgdata/$seqUnloggedPath",         'sequence main fork exists');

# Test the sequence
is($node->safe_psql('postgres', "SELECT nextval('seq_unlogged')"),
	1, 'sequence nextval');
is($node->safe_psql('postgres', "SELECT nextval('seq_unlogged')"),
	2, 'sequence nextval again');

# Create an unlogged table in a tablespace.

my $tablespaceDir = PostgreSQL::Test::Utils::tempdir;

$node->safe_psql('postgres',
	"CREATE TABLESPACE ts1 LOCATION '$tablespaceDir'");
$node->safe_psql('postgres',
	'CREATE UNLOGGED TABLE ts1_unlogged (id int) TABLESPACE ts1');

my $ts1UnloggedPath = $node->safe_psql('postgres',
	q{select pg_relation_filepath('ts1_unlogged')});

# Test that main and init forks exist.
ok(-f "$pgdata/${ts1UnloggedPath}_init", 'init fork in tablespace exists');
ok(-f "$pgdata/$ts1UnloggedPath",        'main fork in tablespace exists');

# Create more unlogged sequences for testing.
$node->safe_psql('postgres', 'CREATE UNLOGGED SEQUENCE seq_unlogged2');
# This rewrites the sequence relation in AlterSequence().
$node->safe_psql('postgres', 'ALTER SEQUENCE seq_unlogged2 INCREMENT 2');
$node->safe_psql('postgres', "SELECT nextval('seq_unlogged2')");

$node->safe_psql('postgres',
	'CREATE UNLOGGED TABLE tab_seq_unlogged3 (a int GENERATED ALWAYS AS IDENTITY)'
);
# This rewrites the sequence relation in ResetSequence().
$node->safe_psql('postgres', 'TRUNCATE tab_seq_unlogged3 RESTART IDENTITY');
$node->safe_psql('postgres', 'INSERT INTO tab_seq_unlogged3 DEFAULT VALUES');

# Crash the postmaster.
$node->stop('immediate');

# Write fake forks to test that they are removed during recovery.
append_to_file("$pgdata/${baseUnloggedPath}_vm",  'TEST_VM');
append_to_file("$pgdata/${baseUnloggedPath}_fsm", 'TEST_FSM');

# Remove main fork to test that it is recopied from init.
unlink("$pgdata/${baseUnloggedPath}")
  or BAIL_OUT("could not remove \"${baseUnloggedPath}\": $!");
unlink("$pgdata/${seqUnloggedPath}")
  or BAIL_OUT("could not remove \"${seqUnloggedPath}\": $!");

# the same for the tablespace
append_to_file("$pgdata/${ts1UnloggedPath}_vm",  'TEST_VM');
append_to_file("$pgdata/${ts1UnloggedPath}_fsm", 'TEST_FSM');
unlink("$pgdata/${ts1UnloggedPath}")
  or BAIL_OUT("could not remove \"${ts1UnloggedPath}\": $!");

$node->start;

# check unlogged table in base
ok( -f "$pgdata/${baseUnloggedPath}_init",
	'table init fork in base still exists');
ok(-f "$pgdata/$baseUnloggedPath",
	'table main fork in base recreated at startup');
ok(!-f "$pgdata/${baseUnloggedPath}_vm",
	'vm fork in base removed at startup');
ok( !-f "$pgdata/${baseUnloggedPath}_fsm",
	'fsm fork in base removed at startup');

# check unlogged sequence
ok(-f "$pgdata/${seqUnloggedPath}_init", 'sequence init fork still exists');
ok(-f "$pgdata/$seqUnloggedPath", 'sequence main fork recreated at startup');

# Test the sequence after restart
is($node->safe_psql('postgres', "SELECT nextval('seq_unlogged')"),
	1, 'sequence nextval after restart');
is($node->safe_psql('postgres', "SELECT nextval('seq_unlogged')"),
	2, 'sequence nextval after restart again');

# check unlogged table in tablespace
ok( -f "$pgdata/${ts1UnloggedPath}_init",
	'init fork still exists in tablespace');
ok(-f "$pgdata/$ts1UnloggedPath",
	'main fork in tablespace recreated at startup');
ok( !-f "$pgdata/${ts1UnloggedPath}_vm",
	'vm fork in tablespace removed at startup');
ok( !-f "$pgdata/${ts1UnloggedPath}_fsm",
	'fsm fork in tablespace removed at startup');

# Test other sequences
is($node->safe_psql('postgres', "SELECT nextval('seq_unlogged2')"),
	1, 'altered sequence nextval after restart');
is($node->safe_psql('postgres', "SELECT nextval('seq_unlogged2')"),
	3, 'altered sequence nextval after restart again');

$node->safe_psql('postgres',
	"INSERT INTO tab_seq_unlogged3 VALUES (DEFAULT), (DEFAULT)");
is($node->safe_psql('postgres', "SELECT * FROM tab_seq_unlogged3"),
	"1\n2", 'reset sequence nextval after restart');

done_testing();
