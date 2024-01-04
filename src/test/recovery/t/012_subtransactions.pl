
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Tests dedicated to subtransactions in recovery
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Setup primary node
my $node_primary = PostgreSQL::Test::Cluster->new("primary");
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', qq(
	max_prepared_transactions = 10
	log_checkpoints = true
));
$node_primary->start;
$node_primary->backup('primary_backup');
$node_primary->psql('postgres', "CREATE TABLE t_012_tbl (id int)");

# Setup standby node
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, 'primary_backup',
	has_streaming => 1);
$node_standby->start;

# Switch to synchronous replication
$node_primary->append_conf(
	'postgresql.conf', qq(
	synchronous_standby_names = '*'
));
$node_primary->psql('postgres', "SELECT pg_reload_conf()");

my $psql_out = '';
my $psql_rc = '';

###############################################################################
# Check that replay will correctly set SUBTRANS and properly advance nextXid
# so that it won't conflict with savepoint xids.
###############################################################################

$node_primary->psql(
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

$node_primary->stop;
$node_primary->start;
$node_primary->psql(
	'postgres', "
	-- here we can get xid of previous savepoint if nextXid
	-- wasn't properly advanced
	BEGIN;
	INSERT INTO t_012_tbl VALUES (142);
	ROLLBACK;
	COMMIT PREPARED 'xact_012_1';");

$node_primary->psql(
	'postgres',
	"SELECT count(*) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '6', "Check nextXid handling for prepared subtransactions");

###############################################################################
# Check that replay will correctly set 2PC with more than
# PGPROC_MAX_CACHED_SUBXIDS subtransactions and also show data properly
# on promotion
###############################################################################
$node_primary->psql('postgres', "DELETE FROM t_012_tbl");

# Function borrowed from src/test/regress/sql/hs_primary_extremes.sql
$node_primary->psql(
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
$node_primary->psql(
	'postgres', "
	BEGIN;
	SELECT hs_subxids(127);
	COMMIT;");
$node_primary->wait_for_catchup($node_standby);
$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '8128', "Visible");
$node_primary->stop;
$node_standby->promote;

$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '8128', "Visible");

# restore state
($node_primary, $node_standby) = ($node_standby, $node_primary);
$node_standby->enable_streaming($node_primary);
$node_standby->start;
$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '8128', "Visible");

$node_primary->psql('postgres', "DELETE FROM t_012_tbl");

# Function borrowed from src/test/regress/sql/hs_primary_extremes.sql
$node_primary->psql(
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
$node_primary->psql(
	'postgres', "
	BEGIN;
	SELECT hs_subxids(127);
	PREPARE TRANSACTION 'xact_012_1';");
$node_primary->wait_for_catchup($node_standby);
$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '-1', "Not visible");
$node_primary->stop;
$node_standby->promote;

$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '-1', "Not visible");

# restore state
($node_primary, $node_standby) = ($node_standby, $node_primary);
$node_standby->enable_streaming($node_primary);
$node_standby->start;
$psql_rc = $node_primary->psql('postgres', "COMMIT PREPARED 'xact_012_1'");
is($psql_rc, '0',
	"Restore of PGPROC_MAX_CACHED_SUBXIDS+ prepared transaction on promoted standby"
);

$node_primary->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '8128', "Visible");

$node_primary->psql('postgres', "DELETE FROM t_012_tbl");
$node_primary->psql(
	'postgres', "
	BEGIN;
	SELECT hs_subxids(201);
	PREPARE TRANSACTION 'xact_012_1';");
$node_primary->wait_for_catchup($node_standby);
$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '-1', "Not visible");
$node_primary->stop;
$node_standby->promote;

$node_standby->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '-1', "Not visible");

# restore state
($node_primary, $node_standby) = ($node_standby, $node_primary);
$node_standby->enable_streaming($node_primary);
$node_standby->start;
$psql_rc = $node_primary->psql('postgres', "ROLLBACK PREPARED 'xact_012_1'");
is($psql_rc, '0',
	"Rollback of PGPROC_MAX_CACHED_SUBXIDS+ prepared transaction on promoted standby"
);

$node_primary->psql(
	'postgres',
	"SELECT coalesce(sum(id),-1) FROM t_012_tbl",
	stdout => \$psql_out);
is($psql_out, '-1', "Not visible");

done_testing();
