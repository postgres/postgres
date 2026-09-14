# Copyright (c) 2026, PostgreSQL Global Development Group

# An offline data checksum state change made on one node of a cascading setup
# is reported all the way down the chain.
#
# CheckReplayedDataChecksumState() is reached from xlog_redo() when a
# checkpoint record (XLOG_CHECKPOINT_SHUTDOWN or XLOG_CHECKPOINT_REDO) is
# replayed after consistency has been reached.  Those records are written by
# the root primary only and are relayed verbatim by every intermediate
# standby, so a cascaded standby that was changed offline learns about the
# divergence too.  An intermediate standby's own restartpoints are not
# WAL-logged and cannot surface it.
#
# Note that this makes detection checkpoint-driven: an idle or read-only
# primary surfaces the divergence no sooner than its next checkpoint, so up to
# checkpoint_timeout may pass before the operator sees anything.  Closing that
# window would require comparing the state when a walreceiver connects.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# This test suite is expensive to execute, require PG_TEST_EXTRA to contain
# 'checksum' to run it.
if ($ENV{PG_TEST_EXTRA})
{
	plan skip_all => 'Expensive data checksums test disabled'
	  unless ($ENV{PG_TEST_EXTRA} =~ /\bchecksum(_extended)?\b/);
}
else
{
	plan skip_all => 'Expensive data checksums test disabled';
}

# checkpoint_timeout is deliberately long so that the only checkpoint record in
# this test is the one requested explicitly below.
my $conf = qq(
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 10GB
);

my $primary = PostgreSQL::Test::Cluster->new('cascade_primary');
$primary->init(allows_streaming => 1, no_data_checksums => 1);
$primary->append_conf('postgresql.conf', $conf);
$primary->start;
$primary->safe_psql('postgres',
	'CREATE TABLE t AS SELECT generate_series(1,1000) AS a;');

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('cascade_standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf('postgresql.conf', $conf);
$standby->start;

$standby->backup('backup');
my $cascade = PostgreSQL::Test::Cluster->new('cascade_cascade');
$cascade->init_from_backup($standby, 'backup', has_streaming => 1);
$cascade->append_conf('postgresql.conf', $conf);
$cascade->start;

$primary->wait_for_catchup($standby);
$standby->wait_for_catchup($cascade);

test_checksum_state($primary, 'off');
test_checksum_state($standby, 'off');
test_checksum_state($cascade, 'off');

# Change the cascaded standby offline, and only it.
$cascade->stop;
$cascade->command_ok(
	[ 'pg_checksums', '--enable', '-D', $cascade->data_dir ],
	'pg_checksums enables checksums on the cascaded standby only');

my $logstart = -s $cascade->logfile;
$cascade->start;

test_checksum_state($cascade, 'on');

# Ordinary WAL traffic carries no state, so the cascaded standby keeps
# streaming from a chain whose state is "off" without noticing.
$primary->safe_psql('postgres', 'INSERT INTO t VALUES (1);');
$primary->wait_for_catchup($standby);
$standby->wait_for_catchup($cascade);

is($cascade->safe_psql('postgres', 'SELECT count(*) FROM t;'),
	'1001', 'cascaded standby keeps streaming from a divergent chain');

# Restartpoints on the intermediate standby are not WAL-logged, so this does
# not surface anything on the cascaded standby either.
$standby->safe_psql('postgres', 'CHECKPOINT;');
$primary->safe_psql('postgres', 'INSERT INTO t VALUES (2);');
$primary->wait_for_catchup($standby);
$standby->wait_for_catchup($cascade);

my $log = PostgreSQL::Test::Utils::slurp_file($cascade->logfile, $logstart);
unlike(
	$log,
	qr/data checksum state .* does not match/,
	'an intermediate restartpoint does not surface the divergence');

# A checkpoint on the root primary does: the record is relayed down the whole
# chain, so both the direct and the cascaded standby compare it against their
# own state.  wait_for_catchup() below waits for replay, not just receipt, so
# the record has been through xlog_redo() on both nodes once it returns.
$primary->safe_psql('postgres', 'CHECKPOINT;');
$primary->wait_for_catchup($standby);
$standby->wait_for_catchup($cascade);

$log = PostgreSQL::Test::Utils::slurp_file($cascade->logfile, $logstart);
like(
	$log,
	qr/data checksum state .* does not match/,
	'the cascaded standby reports the divergence on the primary checkpoint');

$cascade->stop;
$standby->stop;
$primary->stop;

done_testing();
