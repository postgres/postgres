
# Copyright (c) 2021, PostgreSQL Global Development Group

# Test CREATE INDEX CONCURRENTLY with concurrent modifications
use strict;
use warnings;

use Config;
use PostgresNode;
use TestLib;

use Test::More tests => 4;

my ($node, $result);

#
# Test set-up
#
$node = PostgresNode->new('CIC_test');
$node->init;
$node->append_conf('postgresql.conf', 'lock_timeout = 180000');
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));
$node->safe_psql('postgres', q(CREATE TABLE tbl(i int)));
$node->safe_psql('postgres', q(CREATE INDEX idx ON tbl(i)));

#
# Stress CIC with pgbench
#

# Run background pgbench with CIC. We cannot mix-in this script into single
# pgbench: CIC will deadlock with itself occasionally.
my $pgbench_out   = '';
my $pgbench_timer = IPC::Run::timeout(180);
my $pgbench_h     = $node->background_pgbench(
	'--no-vacuum --client=1 --transactions=200',
	{
		'002_pgbench_concurrent_cic' => q(
			DROP INDEX CONCURRENTLY idx;
			CREATE INDEX CONCURRENTLY idx ON tbl(i);
			SELECT bt_index_check('idx',true);
		   )
	},
	\$pgbench_out,
	$pgbench_timer);

# Run pgbench.
$node->pgbench(
	'--no-vacuum --client=5 --transactions=200',
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent INSERTs',
	{
		'002_pgbench_concurrent_transaction' => q(
			BEGIN;
			INSERT INTO tbl VALUES(0);
			COMMIT;
		  ),
		'002_pgbench_concurrent_transaction_savepoints' => q(
			BEGIN;
			SAVEPOINT s1;
			INSERT INTO tbl VALUES(0);
			COMMIT;
		  )
	});

$pgbench_h->pump_nb;
$pgbench_h->finish();
$result =
    ($Config{osname} eq "MSWin32")
  ? ($pgbench_h->full_results)[0]
  : $pgbench_h->result(0);
is($result, 0, "pgbench with CIC works");

# done
$node->stop;
done_testing();
