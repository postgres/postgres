# Check safety of CSN machinery for parallel mode.

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests => 2;

my ($node, $updScr, $selScr, $started, $pgb_handle1, $result, $errors);

$node = PostgreSQL::Test::Cluster->new('csntest');
$node->init;
$node->append_conf('postgresql.conf', qq{
					enable_csn_snapshot = on
					csn_snapshot_defer_time = 10
					default_transaction_isolation = 'REPEATABLE READ'

					# force parallel mode.
					max_worker_processes = 64
					max_parallel_workers_per_gather = 16
					max_parallel_workers = 32
					parallel_setup_cost = 1
					parallel_tuple_cost = 0.05
					min_parallel_table_scan_size = 0
});
$node->start;

$node->command_ok([ 'pgbench', '-i', '-s', '1' ], "pgbench initialization ok");
$node->safe_psql('postgres', qq{
	CREATE OR REPLACE FUNCTION cnt() RETURNS integer AS '
		SELECT sum(abalance) FROM pgbench_accounts;
	' LANGUAGE SQL PARALLEL SAFE COST 100000.;
});


$updScr = File::Temp->new();
append_to_file($updScr, q{
	UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid = 1;
});

$selScr = '
	SELECT count(*) AS res FROM (
		SELECT cnt() AS y FROM pgbench_accounts WHERE aid < 20
		GROUP BY (y)
	) AS q;
';

# Launch updates
$pgb_handle1 = $node->pgbench_async(-n, -T => 10, -f => $updScr, 'postgres' );

$errors = 0;
$started = time();
while (time() - $started < 10)
{
	# Check that each worker returns the same sum on balance column.
	$result = $node->safe_psql('postgres', $selScr);
	if ($result ne 1)
	{
		$errors++;
		diag("Workers returned different sums: $result");
	}
}
is($errors, 0, 'isolation between UPDATE and concurrent SELECT workers.');

$node->pgbench_await($pgb_handle1);
$node->stop();