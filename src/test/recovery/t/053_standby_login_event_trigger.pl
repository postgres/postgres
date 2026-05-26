# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Verify that connecting to a standby still works after a login event
# trigger has been created and dropped on the primary.
#
# CREATE EVENT TRIGGER ... ON login sets pg_database.dathasloginevt to
# true on the primary, but DROP EVENT TRIGGER does not clear it -- the
# next login event trigger pass clears the flag lazily on the primary.
# That dangling flag replicates to the standby.  Before the
# RecoveryInProgress() guard in EventTriggerOnLogin(), the standby
# tried to clear the flag itself, which requires AccessExclusiveLock
# on the database object; that lock mode is forbidden during recovery,
# so the new connection died with FATAL.
#
# To keep the test robust the event trigger is set up in a dedicated
# database (regress_login_evt).  All synchronisation helpers below --
# wait_for_replay_catchup() and friends -- connect to "postgres" on
# the primary; if the trigger were created in "postgres" itself, that
# probe connection would enter the cleanup branch on the primary and
# silently clear the flag before the test even runs, making the
# scenario unreproducible.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Set up primary and a streaming standby.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

my $backup_name = 'login_evt_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;

# A dedicated database isolates the dangling dathasloginevt flag from
# any helper that connects to the default "postgres" database.
$primary->safe_psql('postgres', 'CREATE DATABASE regress_login_evt');
$primary->wait_for_replay_catchup($standby);

# Sanity check: the standby can connect to the new database before
# the trigger machinery has touched it.
$standby->safe_psql('regress_login_evt', 'SELECT 1');

# Create and drop a login event trigger inside the dedicated database
# in a single session.  CREATE EVENT TRIGGER sets
# pg_database.dathasloginevt = true for regress_login_evt; mark it
# ENABLE ALWAYS so the scenario matches the original bug report.
# After DROP the flag remains set on disk until a subsequent login on
# the primary clears it; since later helpers only touch the
# "postgres" database, regress_login_evt's flag stays set and
# replicates that way to the standby.
$primary->safe_psql(
	'regress_login_evt', q{
CREATE FUNCTION init_session() RETURNS event_trigger
LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'init_session'; END $$;
CREATE EVENT TRIGGER init_session ON login
    EXECUTE FUNCTION init_session();
ALTER EVENT TRIGGER init_session ENABLE ALWAYS;
DROP EVENT TRIGGER init_session;
DROP FUNCTION init_session();
});

# Wait for the standby to replay the CREATE/DROP catalog state.  This
# probes "postgres", not regress_login_evt, so it does not disturb
# the dangling flag.
$primary->wait_for_replay_catchup($standby);

# The flag remains set in regress_login_evt on both sides.
is( $primary->safe_psql(
		'postgres',
		"SELECT dathasloginevt FROM pg_database WHERE datname = 'regress_login_evt'"
	),
	't',
	'dathasloginevt remains set on primary after DROP EVENT TRIGGER');
is( $standby->safe_psql(
		'postgres',
		"SELECT dathasloginevt FROM pg_database WHERE datname = 'regress_login_evt'"
	),
	't',
	'dathasloginevt replicated to standby');

# A new connection to regress_login_evt on the standby exercises
# EventTriggerOnLogin()'s cleanup branch.  With the
# RecoveryInProgress() guard it succeeds; without it the session
# aborts with a FATAL about AccessExclusiveLock.
my ($ret, $stdout, $stderr) = $standby->psql('regress_login_evt', 'SELECT 1');
is($ret, 0,
	'standby accepts connection to database with dangling dathasloginevt');
unlike(
	$stderr,
	qr/cannot acquire lock mode AccessExclusiveLock/,
	'no AccessExclusiveLock FATAL on standby login');

# Finally exercise the primary-side cleanup that the standby is meant
# to defer to.  Opening a fresh session against regress_login_evt on
# the primary enters EventTriggerOnLogin()'s cleanup branch with the
# trigger list empty; AccessExclusiveLock is allowed outside recovery,
# so the flag is cleared in place.  The in-place update emits a
# XLOG_HEAP_INPLACE record but does not assign an xid or write a
# commit record, so the WAL is not auto-flushed -- force a flush via
# pg_switch_wal() so the record reaches the standby.
$primary->safe_psql('regress_login_evt', 'SELECT 1');
is( $primary->safe_psql(
		'postgres',
		"SELECT dathasloginevt FROM pg_database WHERE datname = 'regress_login_evt'"
	),
	'f',
	'primary clears dathasloginevt on next login after DROP');

$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->wait_for_replay_catchup($standby);
is( $standby->safe_psql(
		'postgres',
		"SELECT dathasloginevt FROM pg_database WHERE datname = 'regress_login_evt'"
	),
	'f',
	'cleared dathasloginevt replicates to standby');

done_testing();
