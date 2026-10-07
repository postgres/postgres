# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that a commit with synchronous_commit = remote_apply completes when
# the standby has wal_receiver_status_interval = 0.  Periodic status updates
# are disabled then, but the walreceiver must still send the apply reply
# requested by the startup process.
#
# Disable timeout-driven keepalives from the walsender and pings from the
# walreceiver, so that they cannot trigger a status reply that would mask
# a missing apply notification.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);

# Configure synchronous replication at startup, so that there is no window
# where the standby is reported as synchronous before the backends know that
# they have to wait.  Use synchronous_commit = local so that the setup does not
# wait for the standby, which is not available yet.
$primary->append_conf(
	'postgresql.conf', qq(
wal_sender_timeout = 0
synchronous_standby_names = '*'
synchronous_commit = local
));
$primary->start;
$primary->safe_psql('postgres', 'CREATE TABLE t (a int)');
$primary->backup('bkp');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf', qq(
wal_receiver_status_interval = 0
wal_receiver_timeout = 0
hot_standby_feedback = off
));
$standby->start;

$primary->poll_query_until(
	'postgres',
	"SELECT count(*) = 1 FROM pg_stat_replication
	 WHERE state = 'streaming' AND sync_state = 'sync'
	   AND flush_lsn IS NOT NULL"
) or die "timed out waiting for standby to become synchronous";

# The commit must complete even with periodic status updates disabled.
# If the standby fails to report that the commit has been applied,
# query_safe() fails when the session timeout expires.
my $bg = $primary->background_psql('postgres');
$bg->query_safe('SET synchronous_commit = remote_apply');
$bg->query_safe('INSERT INTO t VALUES (1)');
pass('remote_apply commit completes with wal_receiver_status_interval = 0');

is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1', 'remote_apply commit is visible on standby');

$bg->quit;

$standby->stop;
$primary->stop;

done_testing();
