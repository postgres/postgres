# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Tests for already-propagated WAL segments ending in incomplete WAL records.

use strict;
use warnings FATAL => 'all';

use FindBin;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Test: Create a physical replica that's missing the last WAL file,
# then restart the primary to create a divergent WAL file and observe
# that the replica replays the "overwrite contrecord" from that new
# file and the standby promotes successfully.

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
# We need these settings for stability of WAL behavior.
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
wal_keep_size = 1GB
));
$node->start;

$node->safe_psql('postgres', 'create table filler (a int, b text)');

# Now consume all remaining room in the current WAL segment, leaving
# space enough only for the start of a largish record.
$node->safe_psql(
	'postgres', q{
DO $$
DECLARE
    wal_segsize int := setting::int FROM pg_settings WHERE name = 'wal_segment_size';
    remain int;
    iters  int := 0;
BEGIN
    LOOP
        INSERT into filler
        select g, repeat(encode(sha256(g::text::bytea), 'hex'), (random() * 15 + 1)::int)
        from generate_series(1, 10) g;

        remain := wal_segsize - (pg_current_wal_insert_lsn() - '0/0') % wal_segsize;
        IF remain < 2 * setting::int from pg_settings where name = 'block_size' THEN
            RAISE log 'exiting after % iterations, % bytes to end of WAL segment', iters, remain;
            EXIT;
        END IF;
        iters := iters + 1;
    END LOOP;
END
$$;
});

my $initfile = $node->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_insert_lsn())');
$node->safe_psql('postgres',
	qq{SELECT pg_logical_emit_message(true, 'test 026', repeat('xyzxz', 123456))}
);
#$node->safe_psql('postgres', qq{create table foo ()});
my $endfile = $node->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_insert_lsn())');
ok($initfile ne $endfile, "$initfile differs from $endfile");

# Now stop abruptly, to avoid a stop checkpoint.  We can remove the tail file
# afterwards, and on startup the large message should be overwritten with new
# contents
$node->stop('immediate');

unlink $node->basedir . "/pgdata/pg_wal/$endfile"
  or die "could not unlink " . $node->basedir . "/pgdata/pg_wal/$endfile: $!";

# OK, create a standby at this spot.
$node->backup_fs_cold('backup');
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
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

# Verify promotion is successful
$node_standby->promote;

$node->stop;
$node_standby->stop;

done_testing();
