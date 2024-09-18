# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Tests statistics handling around restarts, including handling of crashes and
# invalid stats files, as well as restoring stats after "normal" restarts.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Copy;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', "track_functions = 'all'");
$node->start;

my $connect_db = 'postgres';
my $db_under_test = 'test';

# create test objects
$node->safe_psql($connect_db, "CREATE DATABASE $db_under_test");
$node->safe_psql($db_under_test,
	"CREATE TABLE tab_stats_crash_discard_test1 AS SELECT generate_series(1,100) AS a"
);
$node->safe_psql($db_under_test,
	"CREATE FUNCTION func_stats_crash_discard1() RETURNS VOID AS 'select 2;' LANGUAGE SQL IMMUTABLE"
);

# collect object oids
my $dboid = $node->safe_psql($db_under_test,
	"SELECT oid FROM pg_database WHERE datname = '$db_under_test'");
my $funcoid = $node->safe_psql($db_under_test,
	"SELECT 'func_stats_crash_discard1()'::regprocedure::oid");
my $tableoid = $node->safe_psql($db_under_test,
	"SELECT 'tab_stats_crash_discard_test1'::regclass::oid");

# generate stats and flush them
trigger_funcrel_stat();

# verify stats objects exist
my $sect = "initial";
is(have_stats('database', $dboid, 0), 't', "$sect: db stats do exist");
is(have_stats('function', $dboid, $funcoid),
	't', "$sect: function stats do exist");
is(have_stats('relation', $dboid, $tableoid),
	't', "$sect: relation stats do exist");

# regular shutdown
$node->stop();

# backup stats files
my $statsfile = $PostgreSQL::Test::Utils::tmp_check . '/' . "discard_stats1";
ok(!-f "$statsfile", "backup statsfile cannot already exist");

my $datadir = $node->data_dir();
my $og_stats = "$datadir/pg_stat/pgstat.stat";
ok(-f "$og_stats", "origin stats file must exist");
copy($og_stats, $statsfile) or die "Copy failed: $!";


## test discarding of stats file after crash etc

$node->start;

$sect = "copy";
is(have_stats('database', $dboid, 0), 't', "$sect: db stats do exist");
is(have_stats('function', $dboid, $funcoid),
	't', "$sect: function stats do exist");
is(have_stats('relation', $dboid, $tableoid),
	't', "$sect: relation stats do exist");

$node->stop('immediate');

ok(!-f "$og_stats", "no stats file should exist after immediate shutdown");

# copy the old stats back to test we discard stats after crash restart
copy($statsfile, $og_stats) or die "Copy failed: $!";

$node->start;

# stats should have been discarded
$sect = "post immediate";
is(have_stats('database', $dboid, 0), 'f', "$sect: db stats do not exist");
is(have_stats('function', $dboid, $funcoid),
	'f', "$sect: function stats do exist");
is(have_stats('relation', $dboid, $tableoid),
	'f', "$sect: relation stats do not exist");

# get rid of backup statsfile
unlink $statsfile or die "cannot unlink $statsfile $!";


# generate new stats and flush them
trigger_funcrel_stat();

$sect = "post immediate, new";
is(have_stats('database', $dboid, 0), 't', "$sect: db stats do exist");
is(have_stats('function', $dboid, $funcoid),
	't', "$sect: function stats do exist");
is(have_stats('relation', $dboid, $tableoid),
	't', "$sect: relation stats do exist");

# regular shutdown
$node->stop();


## check an invalid stats file is handled

overwrite_file($og_stats, "ZZZZZZZZZZZZZ");

# normal startup and no issues despite invalid stats file
$node->start;

# no stats present due to invalid stats file
$sect = "invalid_overwrite";
is(have_stats('database', $dboid, 0), 'f', "$sect: db stats do not exist");
is(have_stats('function', $dboid, $funcoid),
	'f', "$sect: function stats do not exist");
is(have_stats('relation', $dboid, $tableoid),
	'f', "$sect: relation stats do not exist");


## check invalid stats file starting with valid contents, but followed by
## invalid content is handled.

trigger_funcrel_stat();
$node->stop;
append_file($og_stats, "XYZ");
$node->start;

$sect = "invalid_append";
is(have_stats('database', $dboid, 0), 'f', "$sect: db stats do not exist");
is(have_stats('function', $dboid, $funcoid),
	'f', "$sect: function stats do not exist");
is(have_stats('relation', $dboid, $tableoid),
	'f', "$sect: relation stats do not exist");


## checks related to stats persistency around restarts and resets

# Ensure enough checkpoints to protect against races for test after reset,
# even on very slow machines.
$node->safe_psql($connect_db, "CHECKPOINT; CHECKPOINT;");


## check checkpoint and wal stats are incremented due to restart

my $ckpt_start = checkpoint_stats();
my $wal_start = wal_stats();
$node->restart;

$sect = "post restart";
my $ckpt_restart = checkpoint_stats();
my $wal_restart = wal_stats();

cmp_ok(
	$ckpt_start->{count}, '<',
	$ckpt_restart->{count},
	"$sect: increased checkpoint count");
cmp_ok(
	$wal_start->{records}, '<',
	$wal_restart->{records},
	"$sect: increased wal record count");
cmp_ok($wal_start->{bytes}, '<', $wal_restart->{bytes},
	"$sect: increased wal bytes");
is( $ckpt_start->{reset},
	$ckpt_restart->{reset},
	"$sect: checkpoint stats_reset equal");
is($wal_start->{reset}, $wal_restart->{reset},
	"$sect: wal stats_reset equal");


## Check that checkpoint stats are reset, WAL stats aren't affected

$node->safe_psql($connect_db, "SELECT pg_stat_reset_shared('checkpointer')");

$sect = "post ckpt reset";
my $ckpt_reset = checkpoint_stats();
my $wal_ckpt_reset = wal_stats();

cmp_ok($ckpt_restart->{count},
	'>', $ckpt_reset->{count}, "$sect: checkpoint count smaller");
cmp_ok($ckpt_start->{reset}, 'lt', $ckpt_reset->{reset},
	"$sect: stats_reset newer");

cmp_ok(
	$wal_restart->{records},
	'<=',
	$wal_ckpt_reset->{records},
	"$sect: wal record count not affected by reset");
is( $wal_start->{reset},
	$wal_ckpt_reset->{reset},
	"$sect: wal stats_reset equal");


## check that checkpoint stats stay reset after restart

$node->restart;

$sect = "post ckpt reset & restart";
my $ckpt_restart_reset = checkpoint_stats();
my $wal_restart2 = wal_stats();

# made sure above there's enough checkpoints that this will be stable even on slow machines
cmp_ok(
	$ckpt_restart_reset->{count},
	'<',
	$ckpt_restart->{count},
	"$sect: checkpoint still reset");
is($ckpt_restart_reset->{reset},
	$ckpt_reset->{reset}, "$sect: stats_reset same");

cmp_ok(
	$wal_ckpt_reset->{records},
	'<',
	$wal_restart2->{records},
	"$sect: increased wal record count");
cmp_ok(
	$wal_ckpt_reset->{bytes},
	'<',
	$wal_restart2->{bytes},
	"$sect: increased wal bytes");
is( $wal_start->{reset},
	$wal_restart2->{reset},
	"$sect: wal stats_reset equal");


## check WAL stats stay reset

$node->safe_psql($connect_db, "SELECT pg_stat_reset_shared('wal')");

$sect = "post wal reset";
my $wal_reset = wal_stats();

cmp_ok(
	$wal_reset->{records}, '<',
	$wal_restart2->{records},
	"$sect: smaller record count");
cmp_ok(
	$wal_reset->{bytes}, '<',
	$wal_restart2->{bytes},
	"$sect: smaller bytes");
cmp_ok(
	$wal_reset->{reset}, 'gt',
	$wal_restart2->{reset},
	"$sect: newer stats_reset");

$node->restart;

$sect = "post wal reset & restart";
my $wal_reset_restart = wal_stats();

# enough WAL generated during prior tests and initdb to make this not racy
cmp_ok(
	$wal_reset_restart->{records},
	'<',
	$wal_restart2->{records},
	"$sect: smaller record count");
cmp_ok(
	$wal_reset->{bytes}, '<',
	$wal_restart2->{bytes},
	"$sect: smaller bytes");
cmp_ok(
	$wal_reset->{reset}, 'gt',
	$wal_restart2->{reset},
	"$sect: newer stats_reset");

$node->stop('immediate');
$node->start;

$sect = "post immediate restart";
my $wal_restart_immediate = wal_stats();

cmp_ok(
	$wal_reset_restart->{reset},
	'lt',
	$wal_restart_immediate->{reset},
	"$sect: reset timestamp is new");

$node->stop;
done_testing();

sub trigger_funcrel_stat
{
	$node->safe_psql(
		$db_under_test, q[
	SELECT * FROM tab_stats_crash_discard_test1;
	SELECT func_stats_crash_discard1();
    SELECT pg_stat_force_next_flush();]);
}

sub have_stats
{
	my ($kind, $dboid, $objid) = @_;

	return $node->safe_psql($connect_db,
		"SELECT pg_stat_have_stats('$kind', $dboid, $objid)");
}

sub overwrite_file
{
	my ($filename, $str) = @_;
	open my $fh, ">", $filename
	  or die "could not overwrite \"$filename\": $!";
	print $fh $str;
	close $fh;
	return;
}

sub append_file
{
	my ($filename, $str) = @_;
	open my $fh, ">>", $filename
	  or die "could not append to \"$filename\": $!";
	print $fh $str;
	close $fh;
	return;
}

sub checkpoint_stats
{
	my %results;

	$results{count} = $node->safe_psql($connect_db,
		"SELECT num_timed + num_requested FROM pg_stat_checkpointer");
	$results{reset} = $node->safe_psql($connect_db,
		"SELECT stats_reset FROM pg_stat_checkpointer");

	return \%results;
}

sub wal_stats
{
	my %results;
	$results{records} =
	  $node->safe_psql($connect_db, "SELECT wal_records FROM pg_stat_wal");
	$results{bytes} =
	  $node->safe_psql($connect_db, "SELECT wal_bytes FROM pg_stat_wal");
	$results{reset} =
	  $node->safe_psql($connect_db, "SELECT stats_reset FROM pg_stat_wal");

	return \%results;
}
