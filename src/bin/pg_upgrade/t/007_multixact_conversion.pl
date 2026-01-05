# Copyright (c) 2025-2026, PostgreSQL Global Development Group

# Version 19 expanded MultiXactOffset from 32 to 64 bits.  Upgrading
# across that requires rewriting the SLRU files to the new format.
# This file contains tests for the conversion.
#
# To run, set 'oldinstall' ENV variable to point to a pre-v19
# installation.  If it's not set, or if it points to a v19 or above
# installation, this still performs a very basic test, upgrading a
# cluster with some multixacts.  It's not very interesting, however,
# because there's no conversion involved in that case.

use strict;
use warnings FATAL => 'all';

use Math::BigInt;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Temp dir for a dumps.
my $tempdir = PostgreSQL::Test::Utils::tempdir;

# A workload that consumes multixids.  The purpose of this is to
# generate some multixids in the old cluster, so that we can test
# upgrading them.  The workload is a mix of KEY SHARE locking queries
# and UPDATEs, and commits and aborts, to generate a mix of multixids
# with different statuses.  It consumes around 3000 multixids with
# 60000 members in total.  That's enough to span more than one
# multixids 'offsets' page, and more than one 'members' segment with
# the default block size.
#
# The workload leaves behind a table called 'mxofftest' containing a
# small number of rows referencing some of the generated multixids.
#
# Because this function is used to generate test data on the old
# installation, it needs to work with older PostgreSQL server
# versions.
#
# The first argument is the cluster to connect to, the second argument
# is a cluster using the new version.  We need the 'psql' binary from
# the new version, the new cluster is otherwise unused.  (We need to
# use the new 'psql' because some of the more advanced background psql
# perl module features depend on a fairly recent psql version.)
sub mxact_workload
{
	my $node = shift;       # Cluster to connect to
	my $binnode = shift;    # Use the psql binary from this cluster

	my $connstr = $node->connstr('postgres');

	$node->start;
	$node->safe_psql(
		'postgres', qq[
		CREATE TABLE mxofftest (id INT PRIMARY KEY, n_updated INT)
		  WITH (AUTOVACUUM_ENABLED=FALSE);
		INSERT INTO mxofftest SELECT G, 0 FROM GENERATE_SERIES(1, 50) G;
	]);

	my $nclients = 20;
	my $update_every = 13;
	my $abort_every = 11;
	my @connections = ();

	# Silence the logging of the statements we run to avoid
	# unnecessarily bloating the test logs.  This runs before the
	# upgrade we're testing, so the details should not be very
	# interesting for debugging.  But if needed, you can make it more
	# verbose by setting this.
	my $verbose = 0;

	# Bump the timeout on the connections to avoid false negatives on
	# slow test systems. The timeout covers the whole duration that
	# the connections are open rather than the individual queries.
	my $connection_timeout_secs =
	  4 * $PostgreSQL::Test::Utils::timeout_default;

	# Open multiple connections to the database.  Start a transaction
	# in each connection.
	for (0 .. $nclients)
	{
		# Use the psql binary from the new installation.  The
		# BackgroundPsql functionality doesn't work with older psql
		# versions.
		my $conn = $binnode->background_psql(
			'',
			connstr => $node->connstr('postgres'),
			timeout => $connection_timeout_secs);

		$conn->query_safe("SET log_statement=none", verbose => $verbose)
		  unless $verbose;
		$conn->query_safe("SET enable_seqscan=off", verbose => $verbose);
		$conn->query_safe("BEGIN", verbose => $verbose);

		push(@connections, $conn);
	}

	# Run queries using cycling through the connections in a
	# round-robin fashion.  We keep a transaction open in each
	# connection at all times, and lock/update the rows.  With 20
	# connections, each SELECT FOR KEY SHARE query generates a new
	# multixid, containing the XIDs of all the transactions running at
	# the time, ie. around 20 XIDs.
	for (my $i = 0; $i < 3000; $i++)
	{
		note "generating multixids $i / 3000\n" if ($i % 100 == 0);

		my $conn = $connections[ $i % $nclients ];

		my $sql = ($i % $abort_every == 0) ? "ABORT" : "COMMIT";
		$conn->query_safe($sql, verbose => $verbose);

		$conn->query_safe("BEGIN", verbose => $verbose);
		if ($i % $update_every == 0)
		{
			$sql = qq[
			  UPDATE mxofftest SET n_updated = n_updated + 1 WHERE id = ${i} % 50;
			];
		}
		else
		{
			my $threshold = int($i / 3000 * 50);
			$sql = qq[
			  select count(*) from (
				SELECT * FROM mxofftest WHERE id >= $threshold FOR KEY SHARE
			  ) as x
			];
		}
		$conn->query_safe($sql, verbose => $verbose);
	}

	for my $conn (@connections)
	{
		$conn->quit();
	}

	$node->stop;
	return;
}

# Return contents of the 'mxofftest' table, created by mxact_workload
sub get_test_table_contents
{
	my ($node, $filename) = @_;

	my $contents = $node->safe_psql('postgres',
		"SELECT ctid, xmin, xmax, * FROM mxofftest");

	my $path = $tempdir . '/' . $filename;
	open(my $fh, '>', $path)
	  || die "could not open $path for writing $!";
	print $fh $contents;
	close($fh);

	return $path;
}

# Return the members of all updating multixids in the given range
sub get_updating_multixact_members
{
	my ($node, $from, $to, $filename) = @_;

	my $path = $tempdir . '/' . $filename;
	open(my $fh, '>', $path)
	  || die "could not open $path for writing $!";

	if ($to >= $from)
	{
		my $res = $node->safe_psql(
			'postgres', qq[
			SELECT multi, mode, xid
			FROM generate_series($from, $to - 1) as multi,
				 pg_get_multixact_members(multi::text::xid)
			WHERE mode not in ('keysh', 'sh');
		]);
		print $fh $res;
	}
	else
	{
		# Multixids wrapped around.  Split the query into two parts,
		# before and after the wraparound.
		my $res = $node->safe_psql(
			'postgres', qq[
			SELECT multi, mode, xid
			FROM generate_series($from, 4294967295) as multi,
				 pg_get_multixact_members(multi::text::xid)
			WHERE mode not in ('keysh', 'sh');
		]);
		print $fh $res;
		$res = $node->safe_psql(
			'postgres', qq[
			SELECT multi, mode, xid
			FROM generate_series(1, $to - 1) as multi,
				 pg_get_multixact_members(multi::text::xid)
			WHERE mode not in ('keysh', 'sh');
		]);
		print $fh $res;
	}

	close($fh);
	return $path;
}

# Read multixid related fields from the control file
#
# Note: This is used on both the old and the new installation, so the
# command arguments and the output parsing used here must work with
# all PostgreSQL versions supported by the test.
sub read_multixid_fields
{
	my $node = shift;

	my $pg_controldata_path = $node->installed_command('pg_controldata');
	my ($stdout, $stderr) =
	  run_command([ $pg_controldata_path, $node->data_dir ]);
	$stdout =~ /^Latest checkpoint's oldestMultiXid:\s*(.*)$/m
	  or die "could not read oldestMultiXid from pg_controldata";
	my $oldest_multi_xid = $1;
	$stdout =~ /^Latest checkpoint's NextMultiXactId:\s*(.*)$/m
	  or die "could not read NextMultiXactId from pg_controldata";
	my $next_multi_xid = $1;
	$stdout =~ /^Latest checkpoint's NextMultiOffset:\s*(.*)$/m
	  or die "could not read NextMultiOffset from pg_controldata";
	my $next_multi_offset = $1;

	return ($oldest_multi_xid, $next_multi_xid, $next_multi_offset);
}

# Reset a cluster's next multixid and mxoffset to given values.
#
# Note: This is used on the old installation, so the command arguments
# and the output parsing used here must work with all pre-v19
# PostgreSQL versions supported by the test.
sub reset_mxid_mxoffset_pre_v19
{
	my $node = shift;
	my $mxid = shift;
	my $mxoffset = shift;

	my $pg_resetwal_path = $node->installed_command('pg_resetwal');
	# Get block size
	my ($out, $err) =
	  run_command([ $pg_resetwal_path, '--dry-run', $node->data_dir ]);
	$out =~ /^Database block size: *(\d+)$/m or die;

	# Verify that no multixids are currently in use.  Resetting would
	# destroy them.  (A freshly initialized cluster has no multixids.)
	$out =~ /^Latest checkpoint's NextMultiXactId: *(\d+)$/m or die;
	my $next_mxid = $1;
	$out =~ /^Latest checkpoint's oldestMultiXid: *(\d+)$/m or die;
	my $oldest_mxid = $1;
	die "cluster has some multixids in use" unless $next_mxid == $oldest_mxid;

	# Extract a few other values from pg_resetwal --dry-run output
	# that we need for the calculations below
	$out =~ /^Database block size: *(\d+)$/m or die;
	my $blcksz = $1;
	# SLRU_PAGES_PER_SEGMENT is always 32 on pre-19 versions
	my $slru_pages_per_segment = 32;

	# Do the reset
	my @cmd = (
		$pg_resetwal_path,
		'--pgdata' => $node->data_dir,
		'--multixact-offset' => $mxoffset,
		'--multixact-ids' => "$mxid,$mxid");
	command_ok(\@cmd, 'reset multixids and offset');

	# pg_resetwal just updates the control file.  The cluster will
	# refuse to start up, if the SLRU segments corresponding to the
	# next multixid and offset does not exist.  Create a segments that
	# covers the given values, filled with zeros.  But first remove
	# any old segments.
	unlink glob $node->data_dir . "/pg_multixact/offsets/*";
	unlink glob $node->data_dir . "/pg_multixact/members/*";

	# Initialize the 'offsets' SLRU file containing the new next multixid
	# with zeros
	#
	# sizeof(MultiXactOffset) == 4 in PostgreSQL versions before 19
	my $multixact_offsets_per_page = $blcksz / 4;
	my $segno =
	  int($mxid / $multixact_offsets_per_page / $slru_pages_per_segment);
	my $path =
	  sprintf('%s/pg_multixact/offsets/%04X', $node->data_dir, $segno);
	open my $fh, ">", $path
	  or die "could not open \"$path\": $!";
	binmode $fh;
	my $bytes_per_seg = $slru_pages_per_segment * $blcksz;
	syswrite($fh, "\0" x $bytes_per_seg) == $bytes_per_seg
	  or die "could not write to \"$path\": $!";
	close $fh;

	# Same for the 'members' SLRU
	my $multixact_members_per_page = int($blcksz / 20) * 4;
	$segno =
	  int($mxoffset / $multixact_members_per_page / $slru_pages_per_segment);
	$path = sprintf "%s/pg_multixact/members/%04X", $node->data_dir, $segno;
	open $fh, ">", $path
	  or die "could not open \"$path\": $!";
	binmode $fh;
	syswrite($fh, "\0" x $bytes_per_seg) == $bytes_per_seg
	  or die "could not write to \"$path\": $!";
	close($fh);
}

# Main test workhorse routine.  Dump data on old version, run
# pg_upgrade, compare data after upgrade.
sub upgrade_and_compare
{
	my $tag = shift;
	my $oldnode = shift;
	my $newnode = shift;

	command_ok(
		[
			'pg_upgrade', '--no-sync',
			'--old-datadir' => $oldnode->data_dir,
			'--new-datadir' => $newnode->data_dir,
			'--old-bindir' => $oldnode->config_data('--bindir'),
			'--new-bindir' => $newnode->config_data('--bindir'),
			'--socketdir' => $newnode->host,
			'--old-port' => $oldnode->port,
			'--new-port' => $newnode->port,
		],
		'run of pg_upgrade for new instance');

	# Dump contents of the test table, and the status of all updating
	# multixids from the old cluster.  (Locking-only multixids don't
	# need to be preserved so we ignore those)
	#
	# Note: we do this *after* running pg_upgrade, to ensure that we
	# don't set all the hint bits before upgrade by doing the SELECT
	# on the table.
	my ($multixids_start, $multixids_end, undef) =
	  read_multixid_fields($oldnode);
	$oldnode->start;
	my $old_table_contents =
	  get_test_table_contents($oldnode, "oldnode_${tag}_table_contents");
	my $old_multixacts =
	  get_updating_multixact_members($oldnode, $multixids_start,
		$multixids_end, "oldnode_${tag}_multixacts");
	$oldnode->stop;

	# Compare them with the upgraded cluster
	$newnode->start;
	my $new_table_contents =
	  get_test_table_contents($newnode, "newnode_${tag}_table_contents");
	my $new_multixacts =
	  get_updating_multixact_members($newnode, $multixids_start,
		$multixids_end, "newnode_${tag}_multixacts");
	$newnode->stop;

	compare_files($old_table_contents, $new_table_contents,
		'test table contents from original and upgraded clusters match');
	compare_files($old_multixacts, $new_multixacts,
		'multixact members from original and upgraded clusters match');
}

my $old_version;

# Basic scenario: Create a cluster using old installation, run
# multixid-creating workload on it, then upgrade.
#
# This works even even if the old and new version is the same,
# although it's not very interesting as the conversion routines only
# run when upgrading from a pre-v19 cluster.
{
	my $tag = 'basic';
	my $old =
	  PostgreSQL::Test::Cluster->new("${tag}_oldnode",
		install_path => $ENV{oldinstall});
	my $new = PostgreSQL::Test::Cluster->new("${tag}_newnode");

	$old->init(extra => ['-k']);

	$old_version = $old->pg_version;
	note "old installation is version $old_version\n";

	# Run the workload
	my (undef, $start_mxid, $start_mxoff) = read_multixid_fields($old);
	mxact_workload($old, $new);
	my (undef, $finish_mxid, $finish_mxoff) = read_multixid_fields($old);

	note "Testing upgrade, ${tag} scenario\n"
	  . " mxid from ${start_mxid} to ${finish_mxid}\n"
	  . " oldnode mxoff from ${start_mxoff} to ${finish_mxoff}\n";

	$new->init;
	upgrade_and_compare($tag, $old, $new);
}

# Wraparound scenario: This is the same as the basic scenario, but the
# old cluster goes through multixid and offset wraparound.
#
# This requires the old installation to be version 18 or older,
# because the hacks we use to reset the old cluster to a state just
# before the wraparound rely on the pre-v19 file format.  If the old
# cluster is of v19 or above, multixact SLRU conversion is not needed
# anyway.
SKIP:
{
	skip
	  "skipping mxoffset conversion tests because upgrading from the old version does not require conversion"
	  if ($old_version >= '19devel');

	my $tag = 'wraparound';
	my $old =
	  PostgreSQL::Test::Cluster->new("${tag}_oldnode",
		install_path => $ENV{oldinstall});
	my $new = PostgreSQL::Test::Cluster->new("${tag}_newnode");

	$old->init(extra => ['-k']);

	# Reset the old cluster to just before multixid and 32-bit offset
	# wraparound.
	reset_mxid_mxoffset_pre_v19($old, 0xFFFFFA00, 0xFFFFEC00);

	# Run the workload.  This crosses multixid and offset wraparound.
	my (undef, $start_mxid, $start_mxoff) = read_multixid_fields($old);
	mxact_workload($old, $new);
	my (undef, $finish_mxid, $finish_mxoff) = read_multixid_fields($old);

	note "Testing upgrade, ${tag} scenario\n"
	  . " mxid from ${start_mxid} to ${finish_mxid}\n"
	  . " oldnode mxoff from ${start_mxoff} to ${finish_mxoff}\n";

	# Verify that wraparounds happened.
	cmp_ok($finish_mxid, '<', $start_mxid,
		"multixid wrapped around in old cluster");
	cmp_ok($finish_mxoff, '<', $start_mxoff,
		"mxoff wrapped around in old cluster");

	$new->init;
	upgrade_and_compare($tag, $old, $new);
}

done_testing();
