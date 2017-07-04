# Testing of commit timestamps preservation across clean restarts
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 16;

my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$node_master->start;

my ($ret, $stdout, $stderr);

($ret, $stdout, $stderr) =
  $node_master->psql('postgres', qq[SELECT pg_xact_commit_timestamp('0');]);
is($ret, 3, 'getting ts of InvalidTransactionId reports error');
like(
	$stderr,
	qr/cannot retrieve commit timestamp for transaction/,
	'expected error from InvalidTransactionId');

($ret, $stdout, $stderr) =
  $node_master->psql('postgres', qq[SELECT pg_xact_commit_timestamp('1');]);
is($ret,    0,  'getting ts of BootstrapTransactionId succeeds');
is($stdout, '', 'timestamp of BootstrapTransactionId is null');

($ret, $stdout, $stderr) =
  $node_master->psql('postgres', qq[SELECT pg_xact_commit_timestamp('2');]);
is($ret,    0,  'getting ts of FrozenTransactionId succeeds');
is($stdout, '', 'timestamp of FrozenTransactionId is null');

# Since FirstNormalTransactionId will've occurred during initdb, long before we
# enabled commit timestamps, it'll be null since we have no cts data for it but
# cts are enabled.
is( $node_master->safe_psql(
		'postgres', qq[SELECT pg_xact_commit_timestamp('3');]),
	'',
	'committs for FirstNormalTransactionId is null');

$node_master->safe_psql('postgres',
	qq[CREATE TABLE committs_test(x integer, y timestamp with time zone);]);

my $xid = $node_master->safe_psql(
	'postgres', qq[
	BEGIN;
	INSERT INTO committs_test(x, y) VALUES (1, current_timestamp);
	SELECT txid_current();
	COMMIT;
]);

my $before_restart_ts = $node_master->safe_psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid');]);
ok($before_restart_ts ne '' && $before_restart_ts ne 'null',
	'commit timestamp recorded');

$node_master->stop('immediate');
$node_master->start;

my $after_crash_ts = $node_master->safe_psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid');]);
is($after_crash_ts, $before_restart_ts,
	'timestamps before and after crash are equal');

$node_master->stop('fast');
$node_master->start;

my $after_restart_ts = $node_master->safe_psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid');]);
is($after_restart_ts, $before_restart_ts,
	'timestamps before and after restart are equal');

# Now disable commit timestamps

$node_master->append_conf('postgresql.conf', 'track_commit_timestamp = off');

$node_master->stop('fast');
$node_master->start;

($ret, $stdout, $stderr) = $node_master->psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid');]);
is($ret, 3, 'no commit timestamp from enable tx when cts disabled');
like(
	$stderr,
	qr/could not get commit timestamp data/,
	'expected error from enabled tx when committs disabled');

# Do a tx while cts disabled
my $xid_disabled = $node_master->safe_psql(
	'postgres', qq[
	BEGIN;
	INSERT INTO committs_test(x, y) VALUES (2, current_timestamp);
	SELECT txid_current();
	COMMIT;
]);

# Should be inaccessible
($ret, $stdout, $stderr) = $node_master->psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid_disabled');]);
is($ret, 3, 'no commit timestamp when disabled');
like(
	$stderr,
	qr/could not get commit timestamp data/,
	'expected error from disabled tx when committs disabled');

# Re-enable, restart and ensure we can still get the old timestamps
$node_master->append_conf('postgresql.conf', 'track_commit_timestamp = on');

$node_master->stop('fast');
$node_master->start;


my $after_enable_ts = $node_master->safe_psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid');]);
is($after_enable_ts, '', 'timestamp of enabled tx null after re-enable');

my $after_enable_disabled_ts = $node_master->safe_psql('postgres',
	qq[SELECT pg_xact_commit_timestamp('$xid_disabled');]);
is($after_enable_disabled_ts, '',
	'timestamp of disabled tx null after re-enable');

$node_master->stop;
