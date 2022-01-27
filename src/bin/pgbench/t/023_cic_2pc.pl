
# Copyright (c) 2021, PostgreSQL Global Development Group

# Test CREATE INDEX CONCURRENTLY with concurrent prepared-xact modifications
use strict;
use warnings;

use Config;
use PostgresNode;
use TestLib;

use Test::More tests => 6;

local $TODO = 'filesystem bug' if TestLib::has_wal_read_bug;

my ($node, $result);

#
# Test set-up
#
$node = get_new_node('CIC_2PC_test');
$node->init;
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 10');
$node->append_conf('postgresql.conf', 'lock_timeout = 180000');
$node->start;
$node->safe_psql('postgres', q(CREATE TABLE tbl(i int)));
$node->safe_psql(
	'postgres', q(
	CREATE FUNCTION heapallindexed() RETURNS void AS $$
	DECLARE
		count_seqscan int;
		count_idxscan int;
	BEGIN
		count_seqscan := (SELECT count(*) FROM tbl);
		SET enable_seqscan = off;
		count_idxscan := (SELECT count(*) FROM tbl);
		RESET enable_seqscan;
		IF count_seqscan <> count_idxscan THEN
			RAISE 'seqscan found % rows, but idxscan found % rows',
				count_seqscan, count_idxscan;
		END IF;
	END
	$$ LANGUAGE plpgsql;
));


#
# Run 3 overlapping 2PC transactions with CIC
#
# We have two concurrent background psql processes: $main_h for INSERTs and
# $cic_h for CIC.  Also, we use non-background psql for some COMMIT PREPARED
# statements.
#

my $main_in    = '';
my $main_out   = '';
my $main_timer = IPC::Run::timeout(180);

my $main_h =
  $node->background_psql('postgres', \$main_in, \$main_out,
	$main_timer, on_error_stop => 1);
$main_in .= q(
BEGIN;
INSERT INTO tbl VALUES(0);
\echo syncpoint1
);
pump $main_h until $main_out =~ /syncpoint1/ || $main_timer->is_expired;

my $cic_in    = '';
my $cic_out   = '';
my $cic_timer = IPC::Run::timeout(180);
my $cic_h =
  $node->background_psql('postgres', \$cic_in, \$cic_out,
	$cic_timer, on_error_stop => 1);
$cic_in .= q(
\echo start
CREATE INDEX CONCURRENTLY idx ON tbl(i);
);
pump $cic_h until $cic_out =~ /start/ || $cic_timer->is_expired;

$main_in .= q(
PREPARE TRANSACTION 'a';
);

$main_in .= q(
BEGIN;
INSERT INTO tbl VALUES(0);
\echo syncpoint2
);
pump $main_h until $main_out =~ /syncpoint2/ || $main_timer->is_expired;

$node->safe_psql('postgres', q(COMMIT PREPARED 'a';));

$main_in .= q(
PREPARE TRANSACTION 'b';
BEGIN;
INSERT INTO tbl VALUES(0);
\echo syncpoint3
);
pump $main_h until $main_out =~ /syncpoint3/ || $main_timer->is_expired;

$node->safe_psql('postgres', q(COMMIT PREPARED 'b';));

$main_in .= q(
PREPARE TRANSACTION 'c';
COMMIT PREPARED 'c';
);
$main_h->pump_nb;

$main_h->finish;
$cic_h->finish;

$result = $node->psql('postgres',
	q(BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT heapallindexed()));
is($result, '0', 'all indexed after overlapping 2PC');


#
# Server restart shall not change whether prepared xact blocks CIC
#

$node->safe_psql(
	'postgres', q(
BEGIN;
INSERT INTO tbl VALUES(0);
PREPARE TRANSACTION 'spans_restart';
BEGIN;
CREATE TABLE unused ();
PREPARE TRANSACTION 'persists_forever';
));
$node->restart;

my $reindex_in    = '';
my $reindex_out   = '';
my $reindex_timer = IPC::Run::timeout(180);
my $reindex_h =
  $node->background_psql('postgres', \$reindex_in, \$reindex_out,
	$reindex_timer, on_error_stop => 1);
$reindex_in .= q(
\echo start
DROP INDEX CONCURRENTLY idx;
CREATE INDEX CONCURRENTLY idx ON tbl(i);
);
pump $reindex_h until $reindex_out =~ /start/ || $reindex_timer->is_expired;

$node->safe_psql('postgres', "COMMIT PREPARED 'spans_restart'");
$reindex_h->finish;
$result = $node->psql('postgres',
	q(BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT heapallindexed()));
is($result, '0', 'all indexed after 2PC and restart');


#
# Stress CIC+2PC with pgbench
#

# Fix broken index first
$node->safe_psql('postgres', q(REINDEX TABLE tbl;));

# Run background pgbench with CIC. We cannot mix-in this script into single
# pgbench: CIC will deadlock with itself occasionally.
my $pgbench_out   = '';
my $pgbench_timer = IPC::Run::timeout(180);
my $pgbench_h     = $node->background_pgbench(
	'--no-vacuum --client=1 --transactions=100',
	{
		'002_pgbench_concurrent_cic' => q(
			DROP INDEX CONCURRENTLY idx;
			CREATE INDEX CONCURRENTLY idx ON tbl(i);
			BEGIN ISOLATION LEVEL REPEATABLE READ;
			SELECT heapallindexed();
			ROLLBACK;
		   )
	},
	\$pgbench_out,
	$pgbench_timer);

# Run pgbench.
$node->pgbench(
	'--no-vacuum --client=5 --transactions=100',
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent INSERTs w/ 2PC',
	{
		'002_pgbench_concurrent_2pc' => q(
			BEGIN;
			INSERT INTO tbl VALUES(0);
			PREPARE TRANSACTION 'c:client_id';
			COMMIT PREPARED 'c:client_id';
		  ),
		'002_pgbench_concurrent_2pc_savepoint' => q(
			BEGIN;
			SAVEPOINT s1;
			INSERT INTO tbl VALUES(0);
			PREPARE TRANSACTION 'c:client_id';
			COMMIT PREPARED 'c:client_id';
		  )
	});

$pgbench_h->pump_nb;
$pgbench_h->finish();
unlike($pgbench_out, qr/aborted in command/, "pgbench with CIC works");

# done
$node->stop;
done_testing();
