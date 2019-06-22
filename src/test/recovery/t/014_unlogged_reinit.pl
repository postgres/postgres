# Tests that unlogged tables are properly reinitialized after a crash.
#
# The behavior should be the same when restoring from a backup, but
# that is not tested here.

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 12;

my $node = get_new_node('main');

$node->init;
$node->start;
my $pgdata = $node->data_dir;

# Create an unlogged table to test that forks other than init are not
# copied.
$node->safe_psql('postgres', 'CREATE UNLOGGED TABLE base_unlogged (id int)');

my $baseUnloggedPath = $node->safe_psql('postgres',
	q{select pg_relation_filepath('base_unlogged')});

# Test that main and init forks exist.
ok(-f "$pgdata/${baseUnloggedPath}_init", 'init fork in base exists');
ok(-f "$pgdata/$baseUnloggedPath",        'main fork in base exists');

# Create an unlogged table in a tablespace.

my $tablespaceDir = TestLib::tempdir;

my $realTSDir = TestLib::perl2host($tablespaceDir);

$node->safe_psql('postgres', "CREATE TABLESPACE ts1 LOCATION '$realTSDir'");
$node->safe_psql('postgres',
	'CREATE UNLOGGED TABLE ts1_unlogged (id int) TABLESPACE ts1');

my $ts1UnloggedPath = $node->safe_psql('postgres',
	q{select pg_relation_filepath('ts1_unlogged')});

# Test that main and init forks exist.
ok(-f "$pgdata/${ts1UnloggedPath}_init", 'init fork in tablespace exists');
ok(-f "$pgdata/$ts1UnloggedPath",        'main fork in tablespace exists');

# Crash the postmaster.
$node->stop('immediate');

# Write fake forks to test that they are removed during recovery.
append_to_file("$pgdata/${baseUnloggedPath}_vm",  'TEST_VM');
append_to_file("$pgdata/${baseUnloggedPath}_fsm", 'TEST_FSM');

# Remove main fork to test that it is recopied from init.
unlink("$pgdata/${baseUnloggedPath}")
  or BAIL_OUT("could not remove \"${baseUnloggedPath}\": $!");

# the same for the tablespace
append_to_file("$pgdata/${ts1UnloggedPath}_vm",  'TEST_VM');
append_to_file("$pgdata/${ts1UnloggedPath}_fsm", 'TEST_FSM');
unlink("$pgdata/${ts1UnloggedPath}")
  or BAIL_OUT("could not remove \"${ts1UnloggedPath}\": $!");

$node->start;

# check unlogged table in base
ok(-f "$pgdata/${baseUnloggedPath}_init", 'init fork in base still exists');
ok(-f "$pgdata/$baseUnloggedPath", 'main fork in base recreated at startup');
ok(!-f "$pgdata/${baseUnloggedPath}_vm",
	'vm fork in base removed at startup');
ok( !-f "$pgdata/${baseUnloggedPath}_fsm",
	'fsm fork in base removed at startup');

# check unlogged table in tablespace
ok( -f "$pgdata/${ts1UnloggedPath}_init",
	'init fork still exists in tablespace');
ok(-f "$pgdata/$ts1UnloggedPath",
	'main fork in tablespace recreated at startup');
ok( !-f "$pgdata/${ts1UnloggedPath}_vm",
	'vm fork in tablespace removed at startup');
ok( !-f "$pgdata/${ts1UnloggedPath}_fsm",
	'fsm fork in tablespace removed at startup');
