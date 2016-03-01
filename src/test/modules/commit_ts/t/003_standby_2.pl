# Test master/standby scenario where the track_commit_timestamp GUC is
# repeatedly toggled on and off.
use strict;
use warnings;

use TestLib;
use Test::More tests => 2;
use PostgresNode;

my $bkplabel = 'backup';
my $master = get_new_node('master');
$master->init(allows_streaming => 1);
$master->append_conf('postgresql.conf', qq{
	track_commit_timestamp = on
	max_wal_senders = 5
	wal_level = hot_standby
	});
$master->start;
$master->backup($bkplabel);

my $standby = get_new_node('standby');
$standby->init_from_backup($master, $bkplabel, has_streaming => 1);
$standby->start;

for my $i (1 .. 10)
{
	$master->psql('postgres', "create table t$i()");
}
$master->append_conf('postgresql.conf', 'track_commit_timestamp = off');
$master->restart;
$master->psql('postgres', 'checkpoint');
my $master_lsn = $master->psql('postgres',
	'select pg_current_xlog_location()');
$standby->poll_query_until('postgres',
	qq{SELECT '$master_lsn'::pg_lsn <= pg_last_xlog_replay_location()})
	or die "slave never caught up";

$standby->psql('postgres', 'checkpoint');
$standby->restart;

my $standby_ts = $standby->psql('postgres',
	qq{SELECT ts.* FROM pg_class, pg_xact_commit_timestamp(xmin) AS ts WHERE relname = 't10'});
is($standby_ts, '', "standby does not return a value after restart");

$master->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$master->restart;
$master->append_conf('postgresql.conf', 'track_commit_timestamp = off');
$master->restart;

system_or_bail('pg_ctl', '-w', '-D', $standby->data_dir, 'promote');
$standby->poll_query_until('postgres', "SELECT pg_is_in_recovery() <> true");

$standby->psql('postgres', "create table t11()");
$standby_ts = $standby->psql('postgres',
	qq{SELECT ts.* FROM pg_class, pg_xact_commit_timestamp(xmin) AS ts WHERE relname = 't11'});
isnt($standby_ts, '', "standby gives valid value ($standby_ts) after promotion");
