# Copyright (c) 2023, PostgreSQL Global Development Group
#
# Test we handle interrupted DROP DATABASE correctly.

use strict;
use warnings;
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

is($node->psql('postgres', 'ALTER DATABASE regression_invalid CONNECTION LIMIT 10'),
	2, "can't ALTER invalid database");

# check invalid database can't be used as a template
is( $node->psql('postgres', 'CREATE DATABASE copy_invalid TEMPLATE regression_invalid'),
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
# dropping the database, making it a suitable point to wait.
my $bgpsql_in    = '';
my $bgpsql_out   = '';
my $bgpsql_err   = '';
my $bgpsql_timer = IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default);
my $bgpsql = $node->background_psql('postgres', \$bgpsql_in, \$bgpsql_out,
	$bgpsql_timer, on_error_stop => 0);
$bgpsql_out = '';
$bgpsql_in .= "SELECT pg_backend_pid();\n";

pump_until($bgpsql, $bgpsql_timer, \$bgpsql_out, qr/\d/);

my $pid = $bgpsql_out;
$bgpsql_out = '';

# create the database, prevent drop database via lock held by a 2PC transaction
$bgpsql_in .= qq(
  CREATE DATABASE regression_invalid_interrupt;
  BEGIN;
  LOCK pg_tablespace;
  PREPARE TRANSACTION 'lock_tblspc';
  \\echo done
);

ok(pump_until($bgpsql, $bgpsql_timer, \$bgpsql_out, qr/done/),
	"blocked DROP DATABASE completion");
$bgpsql_out = '';

# Try to drop. This will wait due to the still held lock.
$bgpsql_in .= qq(
  DROP DATABASE regression_invalid_interrupt;
  \\echo DROP DATABASE completed
);
$bgpsql->pump_nb;

# Ensure we're waiting for the lock
$node->poll_query_until('postgres',
	qq(SELECT EXISTS(SELECT * FROM pg_locks WHERE NOT granted AND relation = 'pg_tablespace'::regclass AND mode = 'AccessShareLock');)
);

# and finally interrupt the DROP DATABASE
ok($node->safe_psql('postgres', "SELECT pg_cancel_backend($pid)"),
	"canceling DROP DATABASE");

# wait for cancellation to be processed
ok( pump_until(
		$bgpsql, $bgpsql_timer, \$bgpsql_out, qr/DROP DATABASE completed/),
	"cancel processed");
$bgpsql_out = '';

# verify that connection to the database aren't allowed
is($node->psql('regression_invalid_interrupt', ''),
	2, "can't connect to invalid_interrupt database");

# To properly drop the database, we need to release the lock previously preventing
# doing so.
$bgpsql_in .= qq(
  ROLLBACK PREPARED 'lock_tblspc';
  \\echo ROLLBACK PREPARED
);
ok(pump_until($bgpsql, $bgpsql_timer, \$bgpsql_out, qr/ROLLBACK PREPARED/),
	"unblock DROP DATABASE");
$bgpsql_out = '';

is($node->psql('postgres', "DROP DATABASE regression_invalid_interrupt"),
	0, "DROP DATABASE invalid_interrupt");

$bgpsql_in .= "\\q\n";
$bgpsql->finish();

done_testing();
