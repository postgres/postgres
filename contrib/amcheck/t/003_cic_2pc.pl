
# Copyright (c) 2021, PostgreSQL Global Development Group

# Test CREATE INDEX CONCURRENTLY with concurrent prepared-xact modifications
use strict;
use warnings;

use Config;
use PostgresNode;
use TestLib;

use Test::More tests => 5;

Test::More->builder->todo_start('filesystem bug')
  if TestLib::has_wal_read_bug;

my ($node, $result);

#
# Test set-up
#
$node = get_new_node('CIC_2PC_test');
$node->init;
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 10');
$node->append_conf('postgresql.conf',
	'lock_timeout = ' . (1000 * $TestLib::timeout_default));
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));
$node->safe_psql('postgres', q(CREATE TABLE tbl(i int)));


#
# Run 3 overlapping 2PC transactions with CIC
#
# We have two concurrent background psql processes: $main_h for INSERTs and
# $cic_h for CIC.  Also, we use non-background psql for some COMMIT PREPARED
# statements.
#

my $main_in    = '';
my $main_out   = '';
my $main_timer = IPC::Run::timeout($TestLib::timeout_default);

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
my $cic_timer = IPC::Run::timeout($TestLib::timeout_default);
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

$result = $node->psql('postgres', q(SELECT bt_index_check('idx',true)));
is($result, '0', 'bt_index_check after overlapping 2PC');


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

my $reindex_in  = '';
my $reindex_out = '';
my $reindex_timer =
  IPC::Run::timeout($TestLib::timeout_default);
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
$result = $node->psql('postgres', q(SELECT bt_index_check('idx',true)));
is($result, '0', 'bt_index_check after 2PC and restart');


#
# Stress CIC+2PC with pgbench
#
# pgbench might try to launch more than one instance of the CIC
# transaction concurrently.  That would deadlock, so use an advisory
# lock to ensure only one CIC runs at a time.

# Fix broken index first
$node->safe_psql('postgres', q(REINDEX TABLE tbl;));

# Run pgbench.
$node->pgbench(
	'--no-vacuum --client=5 --transactions=100',
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent INSERTs w/ 2PC and CIC',
	{
		'003_pgbench_concurrent_2pc' => q(
			BEGIN;
			INSERT INTO tbl VALUES(0);
			PREPARE TRANSACTION 'c:client_id';
			COMMIT PREPARED 'c:client_id';
		  ),
		'003_pgbench_concurrent_2pc_savepoint' => q(
			BEGIN;
			SAVEPOINT s1;
			INSERT INTO tbl VALUES(0);
			PREPARE TRANSACTION 'c:client_id';
			COMMIT PREPARED 'c:client_id';
		  ),
		'003_pgbench_concurrent_cic' => q(
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				DROP INDEX CONCURRENTLY idx;
				CREATE INDEX CONCURRENTLY idx ON tbl(i);
				SELECT bt_index_check('idx',true);
				SELECT pg_advisory_unlock(42);
			\endif
		  ),
		'004_pgbench_concurrent_ric' => q(
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				REINDEX INDEX CONCURRENTLY idx;
				SELECT bt_index_check('idx',true);
				SELECT pg_advisory_unlock(42);
			\endif
		  )
	});

$node->stop;
done_testing();
