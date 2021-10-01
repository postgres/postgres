# Copyright (c) 2021, PostgreSQL Global Development Group

# Tests for already-propagated WAL segments ending in incomplete WAL records.

use strict;
use warnings;

use FindBin;
use PostgresNode;
use TestLib;
use Test::More;

plan tests => 3;

# Test: Create a physical replica that's missing the last WAL file,
# then restart the primary to create a divergent WAL file and observe
# that the replica replays the "overwrite contrecord" from that new
# file.

my $node = PostgresNode->get_new_node('primary');
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
my $node_standby = PostgresNode->get_new_node('standby');
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
	qr[successfully skipped missing contrecord at],
	"found log line in standby");

$node->stop;
$node_standby->stop;
