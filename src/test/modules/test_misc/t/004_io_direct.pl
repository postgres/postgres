# Very simple exercise of direct I/O GUC.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Systems that we know to have direct I/O support, and whose typical local
# filesystems support it or at least won't fail with an error.  (illumos should
# probably be in this list, but perl reports it as solaris.  Solaris should not
# be in the list because we don't support its way of turning on direct I/O, and
# even if we did, its version of ZFS rejects it, and OpenBSD just doesn't have
# it.)
if (!grep { $^O eq $_ } qw(aix darwin dragonfly freebsd linux MSWin32 netbsd))
{
	plan skip_all => "no direct I/O support";
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
io_direct = 'data,wal,wal_init'
shared_buffers = '256kB' # tiny to force I/O
wal_level = replica # minimal runs out of shared_buffers when set so tiny
});
$node->start;

# Do some work that is bound to generate shared and local writes and reads as a
# simple exercise.
$node->safe_psql('postgres',
	'create table t1 as select 1 as i from generate_series(1, 10000)');
$node->safe_psql('postgres', 'create table t2count (i int)');
$node->safe_psql(
	'postgres', qq{
begin;
create temporary table t2 as select 1 as i from generate_series(1, 10000);
update t2 set i = i;
insert into t2count select count(*) from t2;
commit;
});
$node->safe_psql('postgres', 'update t1 set i = i');
is( '10000',
	$node->safe_psql('postgres', 'select count(*) from t1'),
	"read back from shared");
is( '10000',
	$node->safe_psql('postgres', 'select * from t2count'),
	"read back from local");
$node->stop('immediate');

$node->start;
is( '10000',
	$node->safe_psql('postgres', 'select count(*) from t1'),
	"read back from shared after crash recovery");
$node->stop;

done_testing();
