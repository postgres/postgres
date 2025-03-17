
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test primary/standby scenario where the track_commit_timestamp GUC is
# repeatedly toggled on and off.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

my $bkplabel = 'backup';
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq{
	track_commit_timestamp = on
	max_wal_senders = 5
	});
$primary->start;
$primary->backup($bkplabel);

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $bkplabel, has_streaming => 1);
$standby->start;

for my $i (1 .. 10)
{
	$primary->safe_psql('postgres', "create table t$i()");
}
$primary->append_conf('postgresql.conf', 'track_commit_timestamp = off');
$primary->restart;
$primary->safe_psql('postgres', 'checkpoint');
my $primary_lsn =
  $primary->safe_psql('postgres', 'select pg_current_wal_lsn()');
$standby->poll_query_until('postgres',
	qq{SELECT '$primary_lsn'::pg_lsn <= pg_last_wal_replay_lsn()})
  or die "standby never caught up";

$standby->safe_psql('postgres', 'checkpoint');
$standby->restart;

my ($psql_ret, $standby_ts_stdout, $standby_ts_stderr) = $standby->psql(
	'postgres',
	qq{SELECT ts.* FROM pg_class, pg_xact_commit_timestamp(xmin) AS ts WHERE relname = 't10'}
);
is($psql_ret, 3, 'expect error when getting commit timestamp after restart');
is($standby_ts_stdout, '', "standby does not return a value after restart");
like(
	$standby_ts_stderr,
	qr/could not get commit timestamp data/,
	'expected err msg after restart');

$primary->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$primary->restart;
$primary->append_conf('postgresql.conf', 'track_commit_timestamp = off');
$primary->restart;

system_or_bail('pg_ctl', '--pgdata' => $standby->data_dir, 'promote');

$standby->safe_psql('postgres', "create table t11()");
my $standby_ts = $standby->safe_psql('postgres',
	qq{SELECT ts.* FROM pg_class, pg_xact_commit_timestamp(xmin) AS ts WHERE relname = 't11'}
);
isnt($standby_ts, '',
	"standby gives valid value ($standby_ts) after promotion");

done_testing();
