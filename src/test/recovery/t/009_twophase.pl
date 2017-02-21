# Tests dedicated to two-phase commit in recovery
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 13;

# Setup master node
my $node_master = get_new_node("master");
$node_master->init(allows_streaming => 1);
$node_master->append_conf('postgresql.conf', qq(
	max_prepared_transactions = 10
	log_checkpoints = true
));
$node_master->start;
$node_master->backup('master_backup');
$node_master->psql('postgres', "CREATE TABLE t_009_tbl (id int)");

# Setup slave node
my $node_slave = get_new_node('slave');
$node_slave->init_from_backup($node_master, 'master_backup', has_streaming => 1);
$node_slave->start;

# Switch to synchronous replication
$node_master->append_conf('postgresql.conf', qq(
	synchronous_standby_names = '*'
));
$node_master->psql('postgres', "SELECT pg_reload_conf()");

my $psql_out = '';
my $psql_rc = '';

###############################################################################
# Check that we can commit and abort transaction after soft restart.
# Here checkpoint happens before shutdown and no WAL replay will occur at next
# startup. In this case postgres re-creates shared-memory state from twophase
# files.
###############################################################################

$node_master->psql('postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (42);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (43);
	PREPARE TRANSACTION 'xact_009_1';
	BEGIN;
	INSERT INTO t_009_tbl VALUES (142);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (143);
	PREPARE TRANSACTION 'xact_009_2';");
$node_master->stop;
$node_master->start;

$psql_rc = $node_master->psql('postgres', "COMMIT PREPARED 'xact_009_1'");
is($psql_rc, '0', 'Commit prepared transaction after restart');

$psql_rc = $node_master->psql('postgres', "ROLLBACK PREPARED 'xact_009_2'");
is($psql_rc, '0', 'Rollback prepared transaction after restart');

###############################################################################
# Check that we can commit and abort after a hard restart.
# At next startup, WAL replay will re-create shared memory state for prepared
# transaction using dedicated WAL records.
###############################################################################

$node_master->psql('postgres', "
	CHECKPOINT;
	BEGIN;
	INSERT INTO t_009_tbl VALUES (42);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (43);
	PREPARE TRANSACTION 'xact_009_1';
	BEGIN;
	INSERT INTO t_009_tbl VALUES (142);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (143);
	PREPARE TRANSACTION 'xact_009_2';");
$node_master->teardown_node;
$node_master->start;

$psql_rc = $node_master->psql('postgres', "COMMIT PREPARED 'xact_009_1'");
is($psql_rc, '0', 'Commit prepared transaction after teardown');

$psql_rc = $node_master->psql('postgres', "ROLLBACK PREPARED 'xact_009_2'");
is($psql_rc, '0', 'Rollback prepared transaction after teardown');

###############################################################################
# Check that WAL replay can handle several transactions with same GID name.
###############################################################################

$node_master->psql('postgres', "
	CHECKPOINT;
	BEGIN;
	INSERT INTO t_009_tbl VALUES (42);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (43);
	PREPARE TRANSACTION 'xact_009_1';
	COMMIT PREPARED 'xact_009_1';
	BEGIN;
	INSERT INTO t_009_tbl VALUES (42);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (43);
	PREPARE TRANSACTION 'xact_009_1';");
$node_master->teardown_node;
$node_master->start;

$psql_rc = $node_master->psql('postgres', "COMMIT PREPARED 'xact_009_1'");
is($psql_rc, '0', 'Replay several transactions with same GID');

###############################################################################
# Check that WAL replay cleans up its shared memory state and releases locks
# while replaying transaction commits.
###############################################################################

$node_master->psql('postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (42);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (43);
	PREPARE TRANSACTION 'xact_009_1';
	COMMIT PREPARED 'xact_009_1';");
$node_master->teardown_node;
$node_master->start;
$psql_rc = $node_master->psql('postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (42);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (43);
	-- This prepare can fail due to conflicting GID or locks conflicts if
	-- replay did not fully cleanup its state on previous commit.
	PREPARE TRANSACTION 'xact_009_1';");
is($psql_rc, '0', "Cleanup of shared memory state for 2PC commit");

$node_master->psql('postgres', "COMMIT PREPARED 'xact_009_1'");

###############################################################################
# Check that WAL replay will cleanup its shared memory state on running slave.
###############################################################################

$node_master->psql('postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (42);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (43);
	PREPARE TRANSACTION 'xact_009_1';
	COMMIT PREPARED 'xact_009_1';");
$node_slave->psql('postgres', "SELECT count(*) FROM pg_prepared_xacts",
	  stdout => \$psql_out);
is($psql_out, '0',
   "Cleanup of shared memory state on running standby without checkpoint");

###############################################################################
# Same as in previous case, but let's force checkpoint on slave between
# prepare and commit to use on-disk twophase files.
###############################################################################

$node_master->psql('postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (42);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (43);
	PREPARE TRANSACTION 'xact_009_1';");
$node_slave->psql('postgres', "CHECKPOINT");
$node_master->psql('postgres', "COMMIT PREPARED 'xact_009_1'");
$node_slave->psql('postgres', "SELECT count(*) FROM pg_prepared_xacts",
	  stdout => \$psql_out);
is($psql_out, '0',
   "Cleanup of shared memory state on running standby after checkpoint");

###############################################################################
# Check that prepared transactions can be committed on promoted slave.
###############################################################################

$node_master->psql('postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (42);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (43);
	PREPARE TRANSACTION 'xact_009_1';");
$node_master->teardown_node;
$node_slave->promote;
$node_slave->poll_query_until('postgres',
	"SELECT NOT pg_is_in_recovery()")
  or die "Timed out while waiting for promotion of standby";

$psql_rc = $node_slave->psql('postgres', "COMMIT PREPARED 'xact_009_1'");
is($psql_rc, '0', "Restore of prepared transaction on promoted slave");

# change roles
($node_master, $node_slave) = ($node_slave, $node_master);
$node_slave->enable_streaming($node_master);
$node_slave->append_conf('recovery.conf', qq(
recovery_target_timeline='latest'
));
$node_slave->start;

###############################################################################
# Check that prepared transactions are replayed after soft restart of standby
# while master is down. Since standby knows that master is down it uses a
# different code path on startup to ensure that the status of transactions is
# consistent.
###############################################################################

$node_master->psql('postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (42);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (43);
	PREPARE TRANSACTION 'xact_009_1';");
$node_master->stop;
$node_slave->restart;
$node_slave->promote;
$node_slave->poll_query_until('postgres',
	"SELECT NOT pg_is_in_recovery()")
  or die "Timed out while waiting for promotion of standby";

$node_slave->psql('postgres', "SELECT count(*) FROM pg_prepared_xacts",
	  stdout => \$psql_out);
is($psql_out, '1',
   "Restore prepared transactions from files with master down");

# restore state
($node_master, $node_slave) = ($node_slave, $node_master);
$node_slave->enable_streaming($node_master);
$node_slave->append_conf('recovery.conf', qq(
recovery_target_timeline='latest'
));
$node_slave->start;
$node_master->psql('postgres', "COMMIT PREPARED 'xact_009_1'");

###############################################################################
# Check that prepared transactions are correctly replayed after slave hard
# restart while master is down.
###############################################################################

$node_master->psql('postgres', "
	BEGIN;
	INSERT INTO t_009_tbl VALUES (242);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (243);
	PREPARE TRANSACTION 'xact_009_1';
	");
$node_master->stop;
$node_slave->teardown_node;
$node_slave->start;
$node_slave->promote;
$node_slave->poll_query_until('postgres',
	"SELECT NOT pg_is_in_recovery()")
  or die "Timed out while waiting for promotion of standby";

$node_slave->psql('postgres', "SELECT count(*) FROM pg_prepared_xacts",
	  stdout => \$psql_out);
is($psql_out, '1',
   "Restore prepared transactions from records with master down");

# restore state
($node_master, $node_slave) = ($node_slave, $node_master);
$node_slave->enable_streaming($node_master);
$node_slave->append_conf('recovery.conf', qq(
recovery_target_timeline='latest'
));
$node_slave->start;
$node_master->psql('postgres', "COMMIT PREPARED 'xact_009_1'");


###############################################################################
# Check for a lock conflict between prepared transaction with DDL inside and replay of
# XLOG_STANDBY_LOCK wal record.
###############################################################################

$node_master->psql('postgres', "
	BEGIN;
	CREATE TABLE t_009_tbl2 (id int);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl2 VALUES (42);
	PREPARE TRANSACTION 'xact_009_1';
	-- checkpoint will issue XLOG_STANDBY_LOCK that can conflict with lock
	-- held by 'create table' statement
	CHECKPOINT;
	COMMIT PREPARED 'xact_009_1';");

$node_slave->psql('postgres', "SELECT count(*) FROM pg_prepared_xacts",
	  stdout => \$psql_out);
is($psql_out, '0', "Replay prepared transaction with DDL");


###############################################################################
# Check that replay will correctly set SUBTRANS and properly advance nextXid
# so that it won't conflict with savepoint xids.
###############################################################################

$node_master->psql('postgres', "
	BEGIN;
	DELETE FROM t_009_tbl;
	INSERT INTO t_009_tbl VALUES (43);
	SAVEPOINT s1;
	INSERT INTO t_009_tbl VALUES (43);
	SAVEPOINT s2;
	INSERT INTO t_009_tbl VALUES (43);
	SAVEPOINT s3;
	INSERT INTO t_009_tbl VALUES (43);
	SAVEPOINT s4;
	INSERT INTO t_009_tbl VALUES (43);
	SAVEPOINT s5;
	INSERT INTO t_009_tbl VALUES (43);
	PREPARE TRANSACTION 'xact_009_1';
	CHECKPOINT;");

$node_master->stop;
$node_master->start;
$node_master->psql('postgres', "
	-- here we can get xid of previous savepoint if nextXid
	-- wasn't properly advanced
	BEGIN;
	INSERT INTO t_009_tbl VALUES (142);
	ROLLBACK;
	COMMIT PREPARED 'xact_009_1';");

$node_master->psql('postgres', "SELECT count(*) FROM t_009_tbl",
	  stdout => \$psql_out);
is($psql_out, '6', "Check nextXid handling for prepared subtransactions");
