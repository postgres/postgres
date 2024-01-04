
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Testing of commit timestamps preservation across restarts
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$node_primary->start;

my ($ret, $stdout, $stderr);

($ret, $stdout, $stderr) =
  $node_primary->psql('postgres', qq[SELECT pg_xact_commit_timestamp('0');]);
is($ret, 3, 'getting ts of InvalidTransactionId reports error');
like(
	$stderr,
	qr/cannot retrieve commit timestamp for transaction/,
	'expected error from InvalidTransactionId');

($ret, $stdout, $stderr) =
  $node_primary->psql('postgres', qq[SELECT pg_xact_commit_timestamp('1');]);
is($ret, 0, 'getting ts of BootstrapTransactionId succeeds');
is($stdout, '', 'timestamp of BootstrapTransactionId is null');

($ret, $stdout, $stderr) =
  $node_primary->psql('postgres', qq[SELECT pg_xact_commit_timestamp('2');]);
is($ret, 0, 'getting ts of FrozenTransactionId succeeds');
is($stdout, '', 'timestamp of FrozenTransactionId is null');

# Since FirstNormalTransactionId will've occurred during initdb, long before we
# enabled commit timestamps, it'll be null since we have no cts data for it but
# cts are enabled.
is( $node_primary->safe_psql(
		'postgres', qq[SELECT pg_xact_commit_timestamp('3');]),
	'',
	'committs for FirstNormalTransactionId is null');

$node_primary->safe_psql('postgres',
	qq[CREATE TABLE committs_test(x integer, y timestamp with time zone);]);

my $xid = $node_primary->safe_psql(
	'postgres', qq[
	BEGIN;
	INSERT INTO committs_test(x, y) VALUES (1, current_timestamp);
	SELECT pg_current_xact_id()::xid;
	COMMIT;
]);

my $before_restart_ts = $node_primary->safe_psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid');]);
ok($before_restart_ts ne '' && $before_restart_ts ne 'null',
	'commit timestamp recorded');

$node_primary->stop('immediate');
$node_primary->start;

my $after_crash_ts = $node_primary->safe_psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid');]);
is($after_crash_ts, $before_restart_ts,
	'timestamps before and after crash are equal');

$node_primary->stop('fast');
$node_primary->start;

my $after_restart_ts = $node_primary->safe_psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid');]);
is($after_restart_ts, $before_restart_ts,
	'timestamps before and after restart are equal');

# Now disable commit timestamps
$node_primary->append_conf('postgresql.conf', 'track_commit_timestamp = off');
$node_primary->stop('fast');

# Start the server, which generates a XLOG_PARAMETER_CHANGE record where
# the parameter change is registered.
$node_primary->start;

# Now restart again the server so as no XLOG_PARAMETER_CHANGE record are
# replayed with the follow-up immediate shutdown.
$node_primary->restart;

# Move commit timestamps across page boundaries.  Things should still
# be able to work across restarts with those transactions committed while
# track_commit_timestamp is disabled.
$node_primary->safe_psql(
	'postgres',
	qq(CREATE PROCEDURE consume_xid(cnt int)
AS \$\$
DECLARE
    i int;
    BEGIN
        FOR i in 1..cnt LOOP
            EXECUTE 'SELECT pg_current_xact_id()';
            COMMIT;
        END LOOP;
    END;
\$\$
LANGUAGE plpgsql;
));
$node_primary->safe_psql('postgres', 'CALL consume_xid(2000)');

($ret, $stdout, $stderr) = $node_primary->psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid');]);
is($ret, 3, 'no commit timestamp from enable tx when cts disabled');
like(
	$stderr,
	qr/could not get commit timestamp data/,
	'expected error from enabled tx when committs disabled');

# Do a tx while cts disabled
my $xid_disabled = $node_primary->safe_psql(
	'postgres', qq[
	BEGIN;
	INSERT INTO committs_test(x, y) VALUES (2, current_timestamp);
	SELECT pg_current_xact_id();
	COMMIT;
]);

# Should be inaccessible
($ret, $stdout, $stderr) = $node_primary->psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid_disabled');]);
is($ret, 3, 'no commit timestamp when disabled');
like(
	$stderr,
	qr/could not get commit timestamp data/,
	'expected error from disabled tx when committs disabled');

# Re-enable, restart and ensure we can still get the old timestamps
$node_primary->append_conf('postgresql.conf', 'track_commit_timestamp = on');

# An immediate shutdown is used here.  At next startup recovery will
# replay transactions which committed when track_commit_timestamp was
# disabled, and the facility should be able to work properly.
$node_primary->stop('immediate');
$node_primary->start;

my $after_enable_ts = $node_primary->safe_psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid');]);
is($after_enable_ts, '', 'timestamp of enabled tx null after re-enable');

my $after_enable_disabled_ts = $node_primary->safe_psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid_disabled');]);
is($after_enable_disabled_ts, '',
	'timestamp of disabled tx null after re-enable');

$node_primary->stop;

done_testing();
