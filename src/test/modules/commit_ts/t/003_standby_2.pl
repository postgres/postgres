# Test master/standby scenario where the track_commit_timestamp GUC is
# repeatedly toggled on and off.
use strict;
use warnings;

use TestLib;
use Test::More tests => 4;
use PostgresNode;

my $bkplabel = 'backup';
my $master   = get_new_node('master');
$master->init(allows_streaming => 1);
$master->append_conf(
	'postgresql.conf', qq{
	track_commit_timestamp = on
	max_wal_senders = 5
	});
$master->start;
$master->backup($bkplabel);

my $standby = get_new_node('standby');
$standby->init_from_backup($master, $bkplabel, has_streaming => 1);
$standby->start;

for my $i (1 .. 10)
{
	$master->safe_psql('postgres', "create table t$i()");
}
$master->append_conf('postgresql.conf', 'track_commit_timestamp = off');
$master->restart;
$master->safe_psql('postgres', 'checkpoint');
my $master_lsn =
  $master->safe_psql('postgres', 'select pg_current_wal_lsn()');
$standby->poll_query_until('postgres',
	qq{SELECT '$master_lsn'::pg_lsn <= pg_last_wal_replay_lsn()})
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

$master->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$master->restart;
$master->append_conf('postgresql.conf', 'track_commit_timestamp = off');
$master->restart;

system_or_bail('pg_ctl', '-D', $standby->data_dir, 'promote');

$standby->safe_psql('postgres', "create table t11()");
my $standby_ts = $standby->safe_psql('postgres',
qq{SELECT ts.* FROM pg_class, pg_xact_commit_timestamp(xmin) AS ts WHERE relname = 't11'}
);
isnt($standby_ts, '',
	"standby gives valid value ($standby_ts) after promotion");
