
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

# Very simple exercise of direct I/O GUC.

use strict;
use warnings FATAL => 'all';
use Fcntl;
use IO::File;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# We know that macOS has F_NOCACHE, and we know that Windows has
# FILE_FLAG_NO_BUFFERING, and we assume that their typical file systems will
# accept those flags.  For every other system, we'll probe for O_DIRECT
# support.

if ($^O ne 'darwin' && $^O ne 'MSWin32')
{
	# Perl's Fcntl module knows if this system has O_DIRECT in <fcntl.h>.
	if (defined &O_DIRECT)
	{
		# Can we open a file in O_DIRECT mode in the file system where
		# tmp_check lives?
		my $f = IO::File->new(
			"${PostgreSQL::Test::Utils::tmp_check}/test_o_direct_file",
			O_RDWR | O_DIRECT | O_CREAT);
		if (!$f)
		{
			plan skip_all =>
			  "pre-flight test if we can open a file with O_DIRECT failed: $!";
		}
		$f->close;
	}
	else
	{
		plan skip_all => "no O_DIRECT";
	}
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
debug_io_direct = 'data,wal,wal_init'
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
