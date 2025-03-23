use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests => 5;

my ($node, $test_snapshot, $count1, $count2);
$node = PostgreSQL::Test::Cluster->new('csntest');
$node->init;
$node->append_conf('postgresql.conf', qq{
					enable_csn_snapshot = on
					csn_snapshot_defer_time = 10
					max_prepared_transactions = 10
					});
$node->start;

# Create a table
$node->safe_psql('postgres', 'create table t1(i int, j int)');

# insert test record
$node->safe_psql('postgres', 'insert into t1 values(1,1)');
# export csn snapshot
$test_snapshot = $node->safe_psql('postgres', 'select pg_csn_snapshot_export()');
# insert test record
$node->safe_psql('postgres', 'insert into t1 values(2,1)');

$count1 = $node->safe_psql('postgres', "select count(*) from t1");
is($count1, '2', 'Get right number in normal query');
$count2 = $node->safe_psql('postgres', "
			begin transaction isolation level repeatable read;
			select pg_csn_snapshot_import($test_snapshot);
			select count(*) from t1;
			commit;"
			);

is($count2, '
1', 'Get right number in csn import query');

#prepare transaction test
$node->safe_psql('postgres', "
						begin;
						insert into t1 values(3,1);
						insert into t1 values(3,2);
						prepare	transaction 'pt3';
						");
$node->safe_psql('postgres', "
						begin;
						insert into t1 values(4,1);
						insert into t1 values(4,2);
						prepare	transaction 'pt4';
						");
$node->safe_psql('postgres', "
						begin;
						insert into t1 values(5,1);
						insert into t1 values(5,2);
						prepare	transaction 'pt5';
						");
$node->safe_psql('postgres', "
						begin;
						insert into t1 values(6,1);
						insert into t1 values(6,2);
						prepare	transaction 'pt6';
						");
$node->safe_psql('postgres', "commit prepared 'pt4';");

# restart with enable_csn_snapshot off
$node->append_conf('postgresql.conf', "enable_csn_snapshot = off");
$node->restart;
$node->safe_psql('postgres', "
						insert into t1 values(7,1);
						insert into t1 values(7,2);
						");
$node->safe_psql('postgres', "commit prepared 'pt3';");
$count1 = $node->safe_psql('postgres', "select count(*) from t1");
is($count1, '8', 'Get right number in normal query');


# restart with enable_csn_snapshot on
$node->append_conf('postgresql.conf', "enable_csn_snapshot = on");
$node->restart;
$node->safe_psql('postgres', "
						insert into t1 values(8,1);
						insert into t1 values(8,2);
						");
$node->safe_psql('postgres', "commit prepared 'pt5';");
$count1 = $node->safe_psql('postgres', "select count(*) from t1");
is($count1, '12', 'Get right number in normal query');

# restart with enable_csn_snapshot off
$node->append_conf('postgresql.conf', "enable_csn_snapshot = on");
$node->restart;
$node->safe_psql('postgres', "
						insert into t1 values(9,1);
						insert into t1 values(9,2);
						");
$node->safe_psql('postgres', "commit prepared 'pt6';");

$count1 = $node->safe_psql('postgres', "select count(*) from t1");
is($count1, '16', 'Get right number in normal query');
