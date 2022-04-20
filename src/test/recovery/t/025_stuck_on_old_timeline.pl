
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Testing streaming replication where standby is promoted and a new cascading
# standby (without WAL) is connected to the promoted standby.  Both archiving
# and streaming are enabled, but only the history file is available from the
# archive, so the WAL files all have to be streamed.  Test that the cascading
# standby can follow the new primary (promoted standby).
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use File::Basename;
use FindBin;
use Test::More;

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');

# Set up an archive command that will copy the history file but not the WAL
# files. No real archive command should behave this way; the point is to
# simulate a race condition where the new cascading standby starts up after
# the timeline history file reaches the archive but before any of the WAL files
# get there.
$node_primary->init(allows_streaming => 1, has_archiving => 1);

# Note: consistent use of forward slashes here avoids any escaping problems
# that arise from use of backslashes. That means we need to double-quote all
# the paths in the archive_command
my $perlbin = $^X;
$perlbin =~ s!\\!/!g if $PostgreSQL::Test::Utils::windows_os;
my $archivedir_primary = $node_primary->archive_dir;
$archivedir_primary =~ s!\\!/!g if $PostgreSQL::Test::Utils::windows_os;
$node_primary->append_conf(
	'postgresql.conf', qq(
archive_command = '"$perlbin" "$FindBin::RealBin/cp_history_files" "%p" "$archivedir_primary/%f"'
wal_keep_size=128MB
));
# Make sure that Msys perl doesn't complain about difficulty in setting locale
# when called from the archive_command.
local $ENV{PERL_BADLANG} = 0;
$node_primary->start;

# Take backup from primary
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Create streaming standby linking to primary
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup(
	$node_primary, $backup_name,
	allows_streaming => 1,
	has_streaming    => 1,
	has_archiving    => 1);
$node_standby->start;

# Take backup of standby, use -Xnone so that pg_wal is empty.
$node_standby->backup($backup_name, backup_options => ['-Xnone']);

# Create cascading standby but don't start it yet.
# Must set up both streaming and archiving.
my $node_cascade = PostgreSQL::Test::Cluster->new('cascade');
$node_cascade->init_from_backup($node_standby, $backup_name,
	has_streaming => 1);
$node_cascade->enable_restoring($node_primary);
$node_cascade->append_conf(
	'postgresql.conf', qq(
recovery_target_timeline='latest'
));

# Promote the standby.
$node_standby->promote;

# Wait for promotion to complete
$node_standby->poll_query_until('postgres', "SELECT NOT pg_is_in_recovery();")
  or die "Timed out while waiting for promotion";

# Find next WAL segment to be archived
my $walfile_to_be_archived = $node_standby->safe_psql('postgres',
	"SELECT pg_walfile_name(pg_current_wal_lsn());");

# Make WAL segment eligible for archival
$node_standby->safe_psql('postgres', 'SELECT pg_switch_wal()');

# Wait until the WAL segment has been archived.
# Since the history file gets created on promotion and is archived before any
# WAL segment, this is enough to guarantee that the history file was
# archived.
my $archive_wait_query =
  "SELECT '$walfile_to_be_archived' <= last_archived_wal FROM pg_stat_archiver";
$node_standby->poll_query_until('postgres', $archive_wait_query)
  or die "Timed out while waiting for WAL segment to be archived";
my $last_archived_wal_file = $walfile_to_be_archived;

# Start cascade node
$node_cascade->start;

# Create some content on promoted standby and check its presence on the
# cascading standby.
$node_standby->safe_psql('postgres', "CREATE TABLE tab_int AS SELECT 1 AS a");

# Wait for the replication to catch up
$node_standby->wait_for_catchup($node_cascade);

# Check that cascading standby has the new content
my $result =
  $node_cascade->safe_psql('postgres', "SELECT count(*) FROM tab_int");
print "cascade: $result\n";
is($result, 1, 'check streamed content on cascade standby');

done_testing();
