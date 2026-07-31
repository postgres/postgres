# Copyright (c) 2021-2026, PostgreSQL Global Development Group
#
# In the original version of the WAL summarizer code, we were only willing
# to read WAL for a given TLI from a file with that exact TLI encoded into
# the filename. This could result in WAL summarization running on an archiving
# standby getting stuck.
#
# The reason for the problem is that when a new primary is promoted, the
# partial file that ends the old timeline is renamed, giving it a ".partial"
# suffix, meaning that it will be ignored by both recovery and by the WAL
# summarizer. The bytes that appear at the start of that segment will be copied
# into the first segment on the new timeline, and recovery was able to read
# them from there and work as expected. However, the WAL summarizer was
# unwilling to do the same thing, so it got stuck. This test aims to validate
# that this bug has been fixed.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Set up node1 as primary.
my $node1 = PostgreSQL::Test::Cluster->new('node1');
$node1->init(allows_streaming => 1);
$node1->append_conf('postgresql.conf', <<EOM);
autovacuum = off
EOM
$node1->start;

# Set up node2 as a standby for node1. Use archive_mode=always, to make sure it
# archives both before and after promotion.
$node1->backup('backup1');
my $node2 = PostgreSQL::Test::Cluster->new('node2');
$node2->init_from_backup($node1, 'backup1', has_streaming => 1);
$node2->enable_archiving();
$node2->append_conf('postgresql.conf', <<EOM);
archive_mode = always
EOM
$node2->start;

# Wait for node2 to catch up.
$node1->wait_for_replay_catchup($node2);

# Set up node3 as a standby for node2. We want it to fetch WAL only from the
# archive, so we clear primary_conninfo. We don't want long delays during the
# test, so we reduce wal_retrieve_retry_interval. We also don't want it to try
# to archive anything to node2's archive, but at the same time, we don't want
# it to remove WAL before we enable WAL summarization. To accomplish that, we
# set archive_command to the empty string.
$node2->backup('backup2');
my $node3 = PostgreSQL::Test::Cluster->new('node3');
$node3->init_from_backup($node2, 'backup2', has_restoring => 1);
$node3->append_conf('postgresql.conf', <<EOM);
primary_conninfo = ''
wal_retrieve_retry_interval = '500ms'
archive_command = ''
EOM
$node3->start;

# Create a new, partially-filled WAL segment on node1.
$node1->safe_psql('postgres', <<EOM);
SELECT pg_switch_wal();
CREATE TABLE dummy ();
EOM
$node1->wait_for_replay_catchup($node2);

# Record the WAL insert LSN on node1, so we can later verify that summarization
# on node3 advances past this point.
my $node1_final_lsn = $node1->safe_psql('postgres',
	'SELECT pg_current_wal_insert_lsn()');

# Promote node2. This creates a timeline switch that node3 must follow.
$node2->promote;
$node2->poll_query_until('postgres', "SELECT pg_is_in_recovery() = 'f';");

# Cause the partial segment to get archived on the *new* timeline.
#
# In more detail: the WAL segment that contains the current insert LSN exists
# on timeline 1, but since all we did is CREATE TABLE dummy (), it wasn't full.
# We're now running on timeline 2, and pg_switch_wal() fills up the rest of the
# segment.  So the full segment should get archived on timeline 2, but not on
# timeline 1. We do a CHECKPOINT here to make sure that the summarizer tries
# to progress.
my $node2_switch_lsn =
	$node2->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node2->safe_psql('postgres', 'CHECKPOINT');

# Wait until replay has reached TLI 2 on node3, and then start the WAL
# summarizer. If node3 is started with the summarizer already enabled, then
# it may try to fetch the partial segment from timeline 1 before it learns
# about timeline 2. If that happens, it will error out, wait 10 seconds, and
# retry, slowing down the test. This avoids that.
#
# Since the pg_switch_wal() above was executed after promotion, its return
# value is past the timeline switch point, so once replay reaches it, node3
# must be replaying from TLI 2.
#
# We set log_min_messages=debug1 at the same time we enable WAL summarization
# so that we get useful debug messages if there's any problem.
$node3->poll_query_until('postgres',
	"SELECT pg_last_wal_replay_lsn() >= '$node2_switch_lsn'::pg_lsn")
	or die "TLI 2 not reached on node3";
$node3->append_conf('postgresql.conf', <<EOM);
summarize_wal = on
log_min_messages = debug1
EOM
$node3->reload;

# Wait for WAL summarization on node3 to advance past the pre-promotion LSN.
# If the bug is present, the summarizer gets stuck trying to open the old
# timeline's segment file.
my $result = $node3->poll_query_until('postgres', <<EOM);
SELECT EXISTS (SELECT * FROM pg_available_wal_summaries() WHERE tli = 2)
EOM
ok($result, "WAL summarization on node3 advanced past timeline switch");

# Verify the absence of summaries on timeline 2 starting before the final LSN
# from timeline 1.
my $too_early_summaries = $node3->safe_psql('postgres', <<EOM);
SELECT start_lsn, end_lsn FROM pg_available_wal_summaries()
WHERE tli = 2 AND start_lsn < '$node1_final_lsn' ORDER BY start_lsn
EOM
is($too_early_summaries, '', "no summaries from before LSN $node1_final_lsn");

# Verify the presence of summaries on timeline 2 starting at or after the final
# LSN from timeline 1.
my $summaries = $node3->safe_psql('postgres', <<EOM);
SELECT tli, start_lsn, end_lsn FROM pg_available_wal_summaries()
WHERE tli = 2 AND start_lsn >= '$node1_final_lsn' ORDER BY start_lsn
EOM
my @summary_lines = split(/\n/, $summaries);
ok(@summary_lines > 0, "at least one summary from LSN $node1_final_lsn or later");

# We expect the new summaries to be empty, because we have not actually touched
# any block data (and we disabled autovacuum from the start).
for my $line (@summary_lines)
{
	my ($tli, $start_lsn, $end_lsn) = split(/\|/, $line);
	my $filename = sprintf "%s/pg_wal/summaries/%08s%08s%08s%08s%08s.summary",
	  $node3->data_dir, $tli,
	  split(m@/@, $start_lsn),
	  split(m@/@, $end_lsn);
	my ($stdout, $stderr) = run_command([ 'pg_walsummary', $filename ]);
	is($stdout, '', "pg_walsummary TLI $tli $start_lsn-$end_lsn: no blocks");
	is($stderr, '', "pg_walsummary TLI $tli $start_lsn-$end_lsn: no error");
}

done_testing();
