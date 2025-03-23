# Test simple scenario involving a standby

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests => 6;

my ($master, $bkplabel, $standby, $guc_on_master, $guc_on_standby);

$bkplabel = 'backup';
$master = PostgreSQL::Test::Cluster->new('master');
$master->init(allows_streaming => 1);

$master->append_conf(
	'postgresql.conf', qq{
	enable_csn_snapshot = on
	max_wal_senders = 5
	});
$master->start;
$master->backup($bkplabel);

$standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($master, $bkplabel, has_streaming => 1);
$standby->start;

$master->safe_psql('postgres', "create table t1(i int, j int)");

$guc_on_master = $master->safe_psql('postgres', 'show enable_csn_snapshot');
is($guc_on_master, 'on', "GUC on master");

$guc_on_standby = $standby->safe_psql('postgres', 'show enable_csn_snapshot');
is($guc_on_standby, 'on', "GUC on standby");

$master->append_conf('postgresql.conf', 'enable_csn_snapshot = off');
$master->restart;

$guc_on_master = $master->safe_psql('postgres', 'show enable_csn_snapshot');
is($guc_on_master, 'off', "GUC off master");

$guc_on_standby = $standby->safe_psql('postgres', 'show enable_csn_snapshot');
is($guc_on_standby, 'on', "GUC on standby");

# We consume a large number of transaction,for skip page
for my $i (1 .. 4096) #4096
{
	$master->safe_psql('postgres', "insert into t1 values(1,$i)");
}
$master->safe_psql('postgres', "select pg_sleep(2)");
$master->append_conf('postgresql.conf', 'enable_csn_snapshot = on');
$master->restart;

my $count_standby = $standby->safe_psql('postgres', 'select count(*) from t1');
is($count_standby, '4096', "Ok for siwtch xid-base > csn-base"); #4096

# We consume a large number of transaction,for skip page
for my $i (1 .. 4096) #4096
{
	$master->safe_psql('postgres', "insert into t1 values(1,$i)");
}
$master->safe_psql('postgres', "select pg_sleep(2)");

$master->append_conf('postgresql.conf', 'enable_csn_snapshot = off');
$master->restart;

$count_standby = $standby->safe_psql('postgres', 'select count(*) from t1');
is($count_standby, '8192', "Ok for switch csn-base > xid-base"); #8192