# Copyright (c) 2023-2025, PostgreSQL Global Development Group
#
# Test we handle interrupted DROP DATABASE correctly.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf(
	"postgresql.conf", qq(
autovacuum = off
max_prepared_transactions=5
log_min_duration_statement=0
log_connections=on
log_disconnections=on
));

$node->start;


# First verify that we can't connect to or ALTER an invalid database. Just
# mark the database as invalid ourselves, that's more reliable than hitting the
# required race conditions (see testing further down)...

$node->safe_psql(
	"postgres", qq(
CREATE DATABASE regression_invalid;
UPDATE pg_database SET datconnlimit = -2 WHERE datname = 'regression_invalid';
));

my $psql_stdout = '';
my $psql_stderr = '';

is($node->psql('regression_invalid', '', stderr => \$psql_stderr),
	2, "can't connect to invalid database - error code");
like(
	$psql_stderr,
	qr/FATAL:\s+cannot connect to invalid database "regression_invalid"/,
	"can't connect to invalid database - error message");

is( $node->psql(
		'postgres', 'ALTER DATABASE regression_invalid CONNECTION LIMIT 10'),
	2,
	"can't ALTER invalid database");

# check invalid database can't be used as a template
is( $node->psql(
		'postgres',
		'CREATE DATABASE copy_invalid TEMPLATE regression_invalid'),
	3,
	"can't use invalid database as template");


# Verify that VACUUM ignores an invalid database when computing how much of
# the clog is needed (vac_truncate_clog()). For that we modify the pg_database
# row of the invalid database to have an outdated datfrozenxid.
$psql_stderr = '';
$node->psql(
	'postgres',
	qq(
UPDATE pg_database SET datfrozenxid = '123456' WHERE datname = 'regression_invalid';
DROP TABLE IF EXISTS foo_tbl; CREATE TABLE foo_tbl();
VACUUM FREEZE;),
	stderr => \$psql_stderr);
unlike(
	$psql_stderr,
	qr/some databases have not been vacuumed in over 2 billion transactions/,
	"invalid databases are ignored by vac_truncate_clog");


# But we need to be able to drop an invalid database.
is( $node->psql(
		'postgres', 'DROP DATABASE regression_invalid',
		stdout => \$psql_stdout,
		stderr => \$psql_stderr),
	0,
	"can DROP invalid database");

# Ensure database is gone
is($node->psql('postgres', 'DROP DATABASE regression_invalid'),
	3, "can't drop already dropped database");


# Test that interruption of DROP DATABASE is handled properly. To ensure the
# interruption happens at the appropriate moment, we lock pg_tablespace. DROP
# DATABASE scans pg_tablespace once it has reached the "irreversible" part of
# dropping the database, making it a suitable point to wait.  Since relcache
# init reads pg_tablespace, establish each connection before locking.  This
# avoids a connection-time hang with debug_discard_caches.
my $cancel = $node->background_psql('postgres', on_error_stop => 1);
my $bgpsql = $node->background_psql('postgres', on_error_stop => 0);
my $pid = $bgpsql->query('SELECT pg_backend_pid()');

# create the database, prevent drop database via lock held by a 2PC transaction
$bgpsql->query_safe(
	qq(
  CREATE DATABASE regression_invalid_interrupt;
  BEGIN;
  LOCK pg_tablespace;
  PREPARE TRANSACTION 'lock_tblspc';));

# Try to drop. This will wait due to the still held lock.
$bgpsql->query_until(qr//, "DROP DATABASE regression_invalid_interrupt;\n");


# Once the DROP DATABASE is waiting for the lock, interrupt it.
ok( $cancel->query_safe(
		qq(
	DO \$\$
	BEGIN
		WHILE NOT EXISTS(SELECT * FROM pg_locks WHERE NOT granted AND relation = 'pg_tablespace'::regclass AND mode = 'AccessShareLock') LOOP
			PERFORM pg_sleep(.1);
		END LOOP;
	END\$\$;
	SELECT pg_cancel_backend($pid);)),
	"canceling DROP DATABASE");
$cancel->quit();

# wait for cancellation to be processed
ok( pump_until(
		$bgpsql->{run}, $bgpsql->{timeout},
		\$bgpsql->{stderr}, qr/canceling statement due to user request/),
	"cancel processed");
$bgpsql->{stderr} = '';

# Verify that connections to the database aren't allowed.  The backend checks
# this before relcache init, so the lock won't interfere.
is($node->psql('regression_invalid_interrupt', ''),
	2, "can't connect to invalid_interrupt database");

# To properly drop the database, we need to release the lock previously preventing
# doing so.
$bgpsql->query_safe(qq(ROLLBACK PREPARED 'lock_tblspc'));
$bgpsql->query_safe(qq(DROP DATABASE regression_invalid_interrupt));

$bgpsql->quit();

done_testing();
