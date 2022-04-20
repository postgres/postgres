
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Tests dedicated to two-phase commit in recovery
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $psql_out = '';
my $psql_rc  = '';

sub configure_and_reload
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $parameter) = @_;
	my $name = $node->name;

	$node->append_conf(
		'postgresql.conf', qq(
		$parameter
	));
	$node->psql('postgres', "SELECT pg_reload_conf()", stdout => \$psql_out);
	is($psql_out, 't', "reload node $name with $parameter");
	return;
}

# Set up two nodes, which will alternately be primary and replication standby.

# Setup london node
my $node_london = PostgreSQL::Test::Cluster->new("london");
$node_london->init(allows_streaming => 1);
$node_london->append_conf(
	'postgresql.conf', qq(
	max_prepared_transactions = 10
	log_checkpoints = true
));
$node_london->start;
$node_london->backup('london_backup');

# Setup paris node
my $node_paris = PostgreSQL::Test::Cluster->new('paris');
$node_paris->init_from_backup($node_london, 'london_backup',
	has_streaming => 1);
$node_paris->start;

# Switch to synchronous replication in both directions
configure_and_reload($node_london, "synchronous_standby_names = 'paris'");
configure_and_reload($node_paris,  "synchronous_standby_names = 'london'");

# Set up nonce names for current primary and standby nodes
note "Initially, london is primary and paris is standby";
my ($cur_primary, $cur_standby) = ($node_london, $node_paris);
my $cur_primary_name = $cur_primary->name;

# Create table we'll use in the test transactions
$cur_primary->psql('postgres', "CREATE TABLE t_009_tbl (id int, msg text)");

###############################################################################
# Check that we can commit and abort transaction after soft restart.
# Here checkpoint happens before shutdown and no WAL replay will occur at next
# startup. In this case postgres re-creates shared-memory state from twophase
# files.
###############################################################################

$cur_primary->psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (1, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (2, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_1';
	BEGIN;
	INSERT INTO t_009_tbl VALUES (3, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (4, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_2';");
$cur_primary->stop;
$cur_primary->start;

$psql_rc = $cur_primary->psql('postgres', "COMMIT PREPARED 'xact_009_1'");
is($psql_rc, '0', 'Commit prepared transaction after restart');

$psql_rc = $cur_primary->psql('postgres', "ROLLBACK PREPARED 'xact_009_2'");
is($psql_rc, '0', 'Rollback prepared transaction after restart');

###############################################################################
# Check that we can commit and abort after a hard restart.
# At next startup, WAL replay will re-create shared memory state for prepared
# transaction using dedicated WAL records.
###############################################################################

$cur_primary->psql(
	'postgres', "
	CHECKPOINT;
	BEGIN;
	INSERT INTO t_009_tbl VALUES (5, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (6, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_3';
	BEGIN;
	INSERT INTO t_009_tbl VALUES (7, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (8, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_4';");
$cur_primary->teardown_node;
$cur_primary->start;

$psql_rc = $cur_primary->psql('postgres', "COMMIT PREPARED 'xact_009_3'");
is($psql_rc, '0', 'Commit prepared transaction after teardown');

$psql_rc = $cur_primary->psql('postgres', "ROLLBACK PREPARED 'xact_009_4'");
is($psql_rc, '0', 'Rollback prepared transaction after teardown');

###############################################################################
# Check that WAL replay can handle several transactions with same GID name.
###############################################################################

$cur_primary->psql(
	'postgres', "
	CHECKPOINT;
	BEGIN;
	INSERT INTO t_009_tbl VALUES (9, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (10, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_5';
	COMMIT PREPARED 'xact_009_5';
	BEGIN;
	INSERT INTO t_009_tbl VALUES (11, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (12, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_5';");
$cur_primary->teardown_node;
$cur_primary->start;

$psql_rc = $cur_primary->psql('postgres', "COMMIT PREPARED 'xact_009_5'");
is($psql_rc, '0', 'Replay several transactions with same GID');

###############################################################################
# Check that WAL replay cleans up its shared memory state and releases locks
# while replaying transaction commits.
###############################################################################

$cur_primary->psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (13, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (14, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_6';
	COMMIT PREPARED 'xact_009_6';");
$cur_primary->teardown_node;
$cur_primary->start;
$psql_rc = $cur_primary->psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (15, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (16, 'issued to ${cur_primary_name}');
	-- This prepare can fail due to conflicting GID or locks conflicts if
	-- replay did not fully cleanup its state on previous commit.
	PREPARE TRANSACTION 'xact_009_7';");
is($psql_rc, '0', "Cleanup of shared memory state for 2PC commit");

$cur_primary->psql('postgres', "COMMIT PREPARED 'xact_009_7'");

###############################################################################
# Check that WAL replay will cleanup its shared memory state on running standby.
###############################################################################

$cur_primary->psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (17, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (18, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_8';
	COMMIT PREPARED 'xact_009_8';");
$cur_standby->psql(
	'postgres',
	"SELECT count(*) FROM pg_prepared_xacts",
	stdout => \$psql_out);
is($psql_out, '0',
	"Cleanup of shared memory state on running standby without checkpoint");

###############################################################################
# Same as in previous case, but let's force checkpoint on standby between
# prepare and commit to use on-disk twophase files.
###############################################################################

$cur_primary->psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (19, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (20, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_9';");
$cur_standby->psql('postgres', "CHECKPOINT");
$cur_primary->psql('postgres', "COMMIT PREPARED 'xact_009_9'");
$cur_standby->psql(
	'postgres',
	"SELECT count(*) FROM pg_prepared_xacts",
	stdout => \$psql_out);
is($psql_out, '0',
	"Cleanup of shared memory state on running standby after checkpoint");

###############################################################################
# Check that prepared transactions can be committed on promoted standby.
###############################################################################

$cur_primary->psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (21, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (22, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_10';");
$cur_primary->teardown_node;
$cur_standby->promote;

# change roles
note "Now paris is primary and london is standby";
($cur_primary, $cur_standby) = ($node_paris, $node_london);
$cur_primary_name = $cur_primary->name;

# because london is not running at this point, we can't use syncrep commit
# on this command
$psql_rc = $cur_primary->psql('postgres',
	"SET synchronous_commit = off; COMMIT PREPARED 'xact_009_10'");
is($psql_rc, '0', "Restore of prepared transaction on promoted standby");

# restart old primary as new standby
$cur_standby->enable_streaming($cur_primary);
$cur_standby->start;

###############################################################################
# Check that prepared transactions are replayed after soft restart of standby
# while primary is down. Since standby knows that primary is down it uses a
# different code path on startup to ensure that the status of transactions is
# consistent.
###############################################################################

$cur_primary->psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (23, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (24, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_11';");
$cur_primary->stop;
$cur_standby->restart;
$cur_standby->promote;

# change roles
note "Now london is primary and paris is standby";
($cur_primary, $cur_standby) = ($node_london, $node_paris);
$cur_primary_name = $cur_primary->name;

$cur_primary->psql(
	'postgres',
	"SELECT count(*) FROM pg_prepared_xacts",
	stdout => \$psql_out);
is($psql_out, '1',
	"Restore prepared transactions from files with primary down");

# restart old primary as new standby
$cur_standby->enable_streaming($cur_primary);
$cur_standby->start;

$cur_primary->psql('postgres', "COMMIT PREPARED 'xact_009_11'");

###############################################################################
# Check that prepared transactions are correctly replayed after standby hard
# restart while primary is down.
###############################################################################

$cur_primary->psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (25, 'issued to ${cur_primary_name}');
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (26, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_12';
	");
$cur_primary->stop;
$cur_standby->teardown_node;
$cur_standby->start;
$cur_standby->promote;

# change roles
note "Now paris is primary and london is standby";
($cur_primary, $cur_standby) = ($node_paris, $node_london);
$cur_primary_name = $cur_primary->name;

$cur_primary->psql(
	'postgres',
	"SELECT count(*) FROM pg_prepared_xacts",
	stdout => \$psql_out);
is($psql_out, '1',
	"Restore prepared transactions from records with primary down");

# restart old primary as new standby
$cur_standby->enable_streaming($cur_primary);
$cur_standby->start;

$cur_primary->psql('postgres', "COMMIT PREPARED 'xact_009_12'");

###############################################################################
# Check for a lock conflict between prepared transaction with DDL inside and
# replay of XLOG_STANDBY_LOCK wal record.
###############################################################################

$cur_primary->psql(
	'postgres', "
	BEGIN;
	CREATE TABLE t_009_tbl2 (id int, msg text);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl2 VALUES (27, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_13';
	-- checkpoint will issue XLOG_STANDBY_LOCK that can conflict with lock
	-- held by 'create table' statement
	CHECKPOINT;
	COMMIT PREPARED 'xact_009_13';");

# Ensure that last transaction is replayed on standby.
my $cur_primary_lsn =
  $cur_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
my $caughtup_query =
  "SELECT '$cur_primary_lsn'::pg_lsn <= pg_last_wal_replay_lsn()";
$cur_standby->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby to catch up";

$cur_standby->psql(
	'postgres',
	"SELECT count(*) FROM t_009_tbl2",
	stdout => \$psql_out);
is($psql_out, '1', "Replay prepared transaction with DDL");

###############################################################################
# Check recovery of prepared transaction with DDL inside after a hard restart
# of the primary.
###############################################################################

$cur_primary->psql(
	'postgres', "
	BEGIN;
	CREATE TABLE t_009_tbl3 (id int, msg text);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl3 VALUES (28, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_14';
	BEGIN;
	CREATE TABLE t_009_tbl4 (id int, msg text);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl4 VALUES (29, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_15';");

$cur_primary->teardown_node;
$cur_primary->start;

$psql_rc = $cur_primary->psql('postgres', "COMMIT PREPARED 'xact_009_14'");
is($psql_rc, '0', 'Commit prepared transaction after teardown');

$psql_rc = $cur_primary->psql('postgres', "ROLLBACK PREPARED 'xact_009_15'");
is($psql_rc, '0', 'Rollback prepared transaction after teardown');

###############################################################################
# Check recovery of prepared transaction with DDL inside after a soft restart
# of the primary.
###############################################################################

$cur_primary->psql(
	'postgres', "
	BEGIN;
	CREATE TABLE t_009_tbl5 (id int, msg text);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl5 VALUES (30, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_16';
	BEGIN;
	CREATE TABLE t_009_tbl6 (id int, msg text);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl6 VALUES (31, 'issued to ${cur_primary_name}');
	PREPARE TRANSACTION 'xact_009_17';");

$cur_primary->stop;
$cur_primary->start;

$psql_rc = $cur_primary->psql('postgres', "COMMIT PREPARED 'xact_009_16'");
is($psql_rc, '0', 'Commit prepared transaction after restart');

$psql_rc = $cur_primary->psql('postgres', "ROLLBACK PREPARED 'xact_009_17'");
is($psql_rc, '0', 'Rollback prepared transaction after restart');

###############################################################################
# Verify expected data appears on both servers.
###############################################################################

$cur_primary->psql(
	'postgres',
	"SELECT count(*) FROM pg_prepared_xacts",
	stdout => \$psql_out);
is($psql_out, '0', "No uncommitted prepared transactions on primary");

$cur_primary->psql(
	'postgres',
	"SELECT * FROM t_009_tbl ORDER BY id",
	stdout => \$psql_out);
is( $psql_out, qq{1|issued to london
2|issued to london
5|issued to london
6|issued to london
9|issued to london
10|issued to london
11|issued to london
12|issued to london
13|issued to london
14|issued to london
15|issued to london
16|issued to london
17|issued to london
18|issued to london
19|issued to london
20|issued to london
21|issued to london
22|issued to london
23|issued to paris
24|issued to paris
25|issued to london
26|issued to london},
	"Check expected t_009_tbl data on primary");

$cur_primary->psql(
	'postgres',
	"SELECT * FROM t_009_tbl2",
	stdout => \$psql_out);
is( $psql_out,
	qq{27|issued to paris},
	"Check expected t_009_tbl2 data on primary");

$cur_standby->psql(
	'postgres',
	"SELECT count(*) FROM pg_prepared_xacts",
	stdout => \$psql_out);
is($psql_out, '0', "No uncommitted prepared transactions on standby");

$cur_standby->psql(
	'postgres',
	"SELECT * FROM t_009_tbl ORDER BY id",
	stdout => \$psql_out);
is( $psql_out, qq{1|issued to london
2|issued to london
5|issued to london
6|issued to london
9|issued to london
10|issued to london
11|issued to london
12|issued to london
13|issued to london
14|issued to london
15|issued to london
16|issued to london
17|issued to london
18|issued to london
19|issued to london
20|issued to london
21|issued to london
22|issued to london
23|issued to paris
24|issued to paris
25|issued to london
26|issued to london},
	"Check expected t_009_tbl data on standby");

$cur_standby->psql(
	'postgres',
	"SELECT * FROM t_009_tbl2",
	stdout => \$psql_out);
is( $psql_out,
	qq{27|issued to paris},
	"Check expected t_009_tbl2 data on standby");

done_testing();
