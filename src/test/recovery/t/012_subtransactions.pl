# Tests dedicated to subtransactions in recovery
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 12;

# Setup master node
my $node_master = get_new_node("master");
$node_master->init(allows_streaming => 1);
$node_master->append_conf(
	'postgresql.conf', qq(
	max_prepared_transactions = 10
	log_checkpoints = true
));
$node_master->start;
$node_master->backup('master_backup');
$node_master->psql('postgres', "CREATE TABLE t_012_tbl (id int)");

# Setup standby node
my $node_standby = get_new_node('standby');
$node_standby->init_from_backup($node_master, 'master_backup',
	has_streaming => 1);
$node_standby->start;

# Switch to synchronous replication
$node_master->append_conf(
	'postgresql.conf', qq(
	synchronous_standby_names = '*'
));
$node_master->psql('postgres', "SELECT pg_reload_conf()");

my $psql_out = '';
my $psql_rc  = '';

###############################################################################
# Check that replay will correctly set SUBTRANS and properly advance nextXid
# so that it won't conflict with savepoint xids.
###############################################################################

$node_master->psql(
	'postgres', "
	BEGIN;
	DELETE FROM t_012_tbl;
	INSERT INTO t_012_tbl VALUES (43);
	SAVEPOINT s1;
	INSERT INTO t_012_tbl VALUES (43);
	SAVEPOINT s2;
	INSERT INTO t_012_tbl VALUES (43);
	SAVEPOINT s3;
	INSERT INTO t_012_tbl VALUES (43);
	SAVEPOINT s4;
	INSERT INTO t_012_tbl VALUES (43);
	SAVEPOINT s5;
	INSERT INTO t_012_tbl VALUES (43);
	PREPARE TRANSACTION 'xact_012_1';
	CHECKPOINT;");

$node_master->stop;
$node_master->start;
$node_master->psql(
	'postgres', "
	-- here we can get xid of previous savepoint if nextXid
	-- wasn't properly advanced
	BEGIN;
	INSERT INTO t_012_tbl VALUES (142);
	ROLLBACK;
	COMMIT PREPARED 'xact_012_1';");

$node_master->psql(
	'postgres',
	"SELECT count(*) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '6', "Check nextXid handling for prepared subtransactions");

###############################################################################
# Check that replay will correctly set 2PC with more than
# PGPROC_MAX_CACHED_SUBXIDS subtransations and also show data properly
# on promotion
###############################################################################
$node_master->psql('postgres', "DELETE FROM t_012_tbl");

# Function borrowed from src/test/regress/sql/hs_primary_extremes.sql
$node_master->psql(
	'postgres', "
    CREATE OR REPLACE FUNCTION hs_subxids (n integer)
    RETURNS void
    LANGUAGE plpgsql
    AS \$\$
    BEGIN
        IF n <= 0 THEN RETURN; END IF;
        INSERT INTO t_012_tbl VALUES (n);
        PERFORM hs_subxids(n - 1);
        RETURN;
    EXCEPTION WHEN raise_exception THEN NULL; END;
    \$\$;");
$node_master->psql(
	'postgres', "
	BEGIN;
	SELECT hs_subxids(127);
	COMMIT;");
$node_master->wait_for_catchup($node_standby, 'replay',
	$node_master->lsn('insert'));
$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '8128', "Visible");
$node_master->stop;
$node_standby->promote;

$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '8128', "Visible");

# restore state
($node_master, $node_standby) = ($node_standby, $node_master);
$node_standby->enable_streaming($node_master);
$node_standby->append_conf(
	'recovery.conf', qq(
recovery_target_timeline='latest'
));
$node_standby->start;
$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '8128', "Visible");

$node_master->psql('postgres', "DELETE FROM t_012_tbl");

# Function borrowed from src/test/regress/sql/hs_primary_extremes.sql
$node_master->psql(
	'postgres', "
    CREATE OR REPLACE FUNCTION hs_subxids (n integer)
    RETURNS void
    LANGUAGE plpgsql
    AS \$\$
    BEGIN
        IF n <= 0 THEN RETURN; END IF;
        INSERT INTO t_012_tbl VALUES (n);
        PERFORM hs_subxids(n - 1);
        RETURN;
    EXCEPTION WHEN raise_exception THEN NULL; END;
    \$\$;");
$node_master->psql(
	'postgres', "
	BEGIN;
	SELECT hs_subxids(127);
	PREPARE TRANSACTION 'xact_012_1';");
$node_master->wait_for_catchup($node_standby, 'replay',
	$node_master->lsn('insert'));
$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '-1', "Not visible");
$node_master->stop;
$node_standby->promote;

$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '-1', "Not visible");

# restore state
($node_master, $node_standby) = ($node_standby, $node_master);
$node_standby->enable_streaming($node_master);
$node_standby->append_conf(
	'recovery.conf', qq(
recovery_target_timeline='latest'
));
$node_standby->start;
$psql_rc = $node_master->psql('postgres', "COMMIT PREPARED 'xact_012_1'");
is($psql_rc, '0',
"Restore of PGPROC_MAX_CACHED_SUBXIDS+ prepared transaction on promoted standby"
);

$node_master->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '8128', "Visible");

$node_master->psql('postgres', "DELETE FROM t_012_tbl");
$node_master->psql(
	'postgres', "
	BEGIN;
	SELECT hs_subxids(201);
	PREPARE TRANSACTION 'xact_012_1';");
$node_master->wait_for_catchup($node_standby, 'replay',
	$node_master->lsn('insert'));
$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '-1', "Not visible");
$node_master->stop;
$node_standby->promote;

$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '-1', "Not visible");

# restore state
($node_master, $node_standby) = ($node_standby, $node_master);
$node_standby->enable_streaming($node_master);
$node_standby->append_conf(
	'recovery.conf', qq(
recovery_target_timeline='latest'
));
$node_standby->start;
$psql_rc = $node_master->psql('postgres', "ROLLBACK PREPARED 'xact_012_1'");
is($psql_rc, '0',
"Rollback of PGPROC_MAX_CACHED_SUBXIDS+ prepared transaction on promoted standby"
);

$node_master->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '-1', "Not visible");
