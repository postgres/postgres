# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings;
use File::Compare;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Set up a new database instance.
my $node1 = PostgreSQL::Test::Cluster->new('node1');
$node1->init(has_archiving => 1, allows_streaming => 1);
$node1->append_conf('postgresql.conf', 'summarize_wal = on');
$node1->start;

# Create a table and insert a few test rows into it. VACUUM FREEZE it so that
# autovacuum doesn't induce any future modifications unexpectedly. Then
# trigger a checkpoint.
$node1->safe_psql('postgres', <<EOM);
CREATE TABLE mytable (a int, b text);
INSERT INTO mytable
SELECT
	g, random()::text||random()::text||random()::text||random()::text
FROM
	generate_series(1, 400) g;
VACUUM FREEZE;
EOM

# Record the current WAL insert LSN.
my $base_lsn = $node1->safe_psql('postgres', <<EOM);
SELECT pg_current_wal_insert_lsn()
EOM
note("just after insert, WAL insert LSN is $base_lsn");

# Now perform a CHECKPOINT.
$node1->safe_psql('postgres', <<EOM);
CHECKPOINT;
EOM

# Wait for a new summary to show up, one that includes the inserts we just did.
my $result = $node1->poll_query_until('postgres', <<EOM);
SELECT EXISTS (
    SELECT * from pg_available_wal_summaries()
    WHERE end_lsn >= '$base_lsn'
)
EOM
ok($result, "WAL summarization caught up after insert");

# Get a list of what summaries we now have.
my $progress = $node1->safe_psql('postgres', <<EOM);
SELECT summarized_tli, summarized_lsn FROM pg_get_wal_summarizer_state()
EOM
my ($summarized_tli, $summarized_lsn) = split(/\|/, $progress);
note("after insert, summarized TLI $summarized_tli through $summarized_lsn");
note_wal_summary_dir("after insert", $node1);

# Update a row in the first block of the table and trigger a checkpoint.
$node1->safe_psql('postgres', <<EOM);
UPDATE mytable SET b = 'abcdefghijklmnopqrstuvwxyz' WHERE a = 2;
CHECKPOINT;
EOM

# Wait for a new summary to show up.
$result = $node1->poll_query_until('postgres', <<EOM);
SELECT EXISTS (
    SELECT * from pg_available_wal_summaries()
    WHERE tli = $summarized_tli AND end_lsn > '$summarized_lsn'
)
EOM
ok($result, "got new WAL summary after update");

# Figure out the exact details for the new summary file.
my $details = $node1->safe_psql('postgres', <<EOM);
SELECT tli, start_lsn, end_lsn from pg_available_wal_summaries()
	WHERE tli = $summarized_tli AND end_lsn > '$summarized_lsn'
EOM
my @lines = split(/\n/, $details);
is(0+@lines, 1, "got exactly one new WAL summary");
my ($tli, $start_lsn, $end_lsn) = split(/\|/, $lines[0]);
note("examining summary for TLI $tli from $start_lsn to $end_lsn");
note_wal_summary_dir("after new summary", $node1);

# Reconstruct the full pathname for the WAL summary file.
my $filename = sprintf "%s/pg_wal/summaries/%08s%08s%08s%08s%08s.summary",
					   $node1->data_dir, $tli,
					   split(m@/@, $start_lsn),
					   split(m@/@, $end_lsn);
ok(-f $filename, "WAL summary file exists");
note_wal_summary_dir("after existence check", $node1);

# Run pg_walsummary on it. We expect exactly two blocks to be modified,
# block 0 and one other.
my ($stdout, $stderr) = run_command([ 'pg_walsummary', '-i', $filename ]);
note($stdout);
@lines = split(/\n/, $stdout);
like($stdout, qr/FORK main: block 0$/m, "stdout shows block 0 modified");
is($stderr, '', 'stderr is empty');
is(0+@lines, 2, "UPDATE modified 2 blocks");
note_wal_summary_dir("after pg_walsummary run", $node1);

done_testing();

# XXX. Temporary debugging code.
sub note_wal_summary_dir
{
	my ($flair, $node) = @_;

	my $wsdir = sprintf "%s/pg_wal/summaries", $node->data_dir;
	my @wsfiles = grep { $_ ne '.' && $_ ne '..' } slurp_dir($wsdir);
	note("$flair pg_wal/summaries has: @wsfiles");
}
