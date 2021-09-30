# Copyright (c) 2021, PostgreSQL Global Development Group

# Tests for already-propagated WAL segments ending in incomplete WAL records.

use strict;
use warnings;

use FindBin;
use PostgresNode;
use TestLib;
use Test::More;

plan tests => 5;

# Test: Create a physical replica that's missing the last WAL file,
# then restart the primary to create a divergent WAL file and observe
# that the replica replays the "overwrite contrecord" from that new
# file.

my $node = PostgresNode->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'wal_keep_size=1GB');
$node->start;

$node->safe_psql('postgres', 'create table filler (a int)');
# First, measure how many bytes does the insertion of 1000 rows produce
my $start_lsn =
  $node->safe_psql('postgres', q{select pg_current_wal_insert_lsn() - '0/0'});
$node->safe_psql('postgres',
	'insert into filler select * from generate_series(1, 1000)');
my $end_lsn =
  $node->safe_psql('postgres', q{select pg_current_wal_insert_lsn() - '0/0'});
my $rows_walsize = $end_lsn - $start_lsn;

# Now consume all remaining room in the current WAL segment, leaving
# space enough only for the start of a largish record.
$node->safe_psql(
	'postgres', qq{
WITH setting AS (
  SELECT setting::int AS wal_segsize
    FROM pg_settings WHERE name = 'wal_segment_size'
)
INSERT INTO filler
SELECT g FROM setting,
  generate_series(1, 1000 * (wal_segsize - ((pg_current_wal_insert_lsn() - '0/0') % wal_segsize)) / $rows_walsize) g
});

my $initfile = $node->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_insert_lsn())');
$node->safe_psql('postgres',
	qq{SELECT pg_logical_emit_message(true, 'test 026', repeat('xyzxz', 123456))}
);
#$node->safe_psql('postgres', qq{create table foo ()});
my $endfile = $node->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_insert_lsn())');
ok($initfile != $endfile, "$initfile differs from $endfile");

# Now stop abruptly, to avoid a stop checkpoint.  We can remove the tail file
# afterwards, and on startup the large message should be overwritten with new
# contents
$node->stop('immediate');

unlink $node->basedir . "/pgdata/pg_wal/$endfile"
  or die "could not unlink " . $node->basedir . "/pgdata/pg_wal/$endfile: $!";

# OK, create a standby at this spot.
$node->backup_fs_cold('backup');
my $node_standby = PostgresNode->new('standby');
$node_standby->init_from_backup($node, 'backup', has_streaming => 1);

$node_standby->start;
$node->start;

$node->safe_psql('postgres',
	qq{create table foo (a text); insert into foo values ('hello')});
$node->safe_psql('postgres',
	qq{SELECT pg_logical_emit_message(true, 'test 026', 'AABBCC')});

my $until_lsn = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
my $caughtup_query =
  "SELECT '$until_lsn'::pg_lsn <= pg_last_wal_replay_lsn()";
$node_standby->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby to catch up";

ok($node_standby->safe_psql('postgres', 'select * from foo') eq 'hello',
	'standby replays past overwritten contrecord');

# Verify message appears in standby's log
my $log = slurp_file($node_standby->logfile);
like(
	$log,
	qr[sucessfully skipped missing contrecord at],
	"found log line in standby");

$node->stop;
$node_standby->stop;


# Second test: a standby that receives WAL via archive/restore commands.
$node = PostgresNode->new('primary2');
$node->init(
	has_archiving => 1,
	extra         => ['--wal-segsize=1']);
$node->set_replication_conf;

# Note: consistent use of forward slashes here avoids any escaping problems
# that arise from use of backslashes. That means we need to double-quote all
# the paths in the archive_command
my $perlbin = TestLib::perl2host($^X);
$perlbin =~ s!\\!/!g if $TestLib::windows_os;
my $archivedir = $node->archive_dir;
$archivedir =~ s!\\!/!g if $TestLib::windows_os;
$node->append_conf(
	'postgresql.conf',
	qq{
archive_command = '"$perlbin" "$FindBin::RealBin/idiosyncratic_copy" "%p" "$archivedir/%f"'
wal_level = replica
max_wal_senders = 2
wal_keep_size = 1GB
});
# Make sure that Msys perl doesn't complain about difficulty in setting locale
# when called from the archive_command.
local $ENV{PERL_BADLANG} = 0;
$node->start;
$node->backup('backup');

$node_standby = PostgresNode->new('standby2');
$node_standby->init_from_backup($node, 'backup', has_restoring => 1);

$node_standby->start;

$node->safe_psql('postgres', 'create table filler (a int)');
# First, measure how many bytes does the insertion of 1000 rows produce
$start_lsn =
  $node->safe_psql('postgres', q{select pg_current_wal_insert_lsn() - '0/0'});
$node->safe_psql('postgres',
	'insert into filler select * from generate_series(1, 1000)');
$end_lsn =
  $node->safe_psql('postgres', q{select pg_current_wal_insert_lsn() - '0/0'});
$rows_walsize = $end_lsn - $start_lsn;

# Now consume all remaining room in the current WAL segment, leaving
# space enough only for the start of a largish record.
$node->safe_psql(
	'postgres', qq{
WITH setting AS (
  SELECT setting::int AS wal_segsize
    FROM pg_settings WHERE name = 'wal_segment_size'
)
INSERT INTO filler
SELECT g FROM setting,
  generate_series(1, 1000 * (wal_segsize - ((pg_current_wal_insert_lsn() - '0/0') % wal_segsize)) / $rows_walsize) g
});

# Now block idiosyncratic_copy from creating the next WAL in the replica
my $archivedgood = $node->safe_psql('postgres',
	q{SELECT pg_walfile_name(pg_current_wal_insert_lsn())});
my $archivedfail = $node->safe_psql(
	'postgres',
	q{SELECT pg_walfile_name(pg_current_wal_insert_lsn() + setting::integer)
	from pg_settings where name = 'wal_segment_size'});
open my $filefail, ">", "$archivedir/$archivedfail.fail"
  or die "can't open $archivedir/$archivedfail.fail: $!";

my $currlsn =
  $node->safe_psql('postgres', 'select pg_current_wal_insert_lsn() - 1000');

# Now produce a large WAL record in a transaction that we leave open
my ($in, $out);
my $timer = IPC::Run::timeout(180);
my $h =
  $node->background_psql('postgres', \$in, \$out, $timer, on_error_stop => 0);

$in .= qq{BEGIN;
SELECT pg_logical_emit_message(true, 'test 026', repeat('somenoise', 8192));
};
$h->pump_nb;
$node->poll_query_until(
	'postgres',
	"SELECT last_archived_wal >= '$archivedgood' FROM pg_stat_archiver"),
  or die "Timed out while waiting for standby to catch up";

# Now crash the node with the transaction open
$node->stop('immediate');
$h->finish();
$node->start;
$node->safe_psql('postgres', 'create table witness (a int);');
$node->safe_psql('postgres', 'insert into witness values (42)');
unlink "$archivedir/$archivedfail.fail"
  or die "can't unlink $archivedir/$archivedfail.fail: $!";
$node->safe_psql('postgres', 'select pg_switch_wal()');

$until_lsn = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
$caughtup_query = "SELECT '$until_lsn'::pg_lsn <= pg_last_wal_replay_lsn()";
$node_standby->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby to catch up";

my $answer = $node_standby->safe_psql('postgres', 'select * from witness');
is($answer, '42', 'witness tuple appears');

# Verify message appears in standby's log
$log = slurp_file($node_standby->logfile);
like(
	$log,
	qr[sucessfully skipped missing contrecord at],
	"found log line in standby");
$node->stop;
$node_standby->stop;
