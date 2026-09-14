# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash and checkpoint windows inside an online data checksum enable on
# a single node.  Each scenario starts from "off" on the same cluster,
# runs the enable into a held injection point, and checks what a crash
# or a concurrent checkpoint in that window may and may not do to the
# control file and the pages on disk.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

use IPC::Run;

# This test suite is expensive to execute, require PG_TEST_EXTRA to contain
# 'checksum_extended' to run it.
if ($ENV{PG_TEST_EXTRA})
{
	plan skip_all => 'Expensive data checksums test disabled'
	  unless ($ENV{PG_TEST_EXTRA} =~ /\bchecksum_extended\b/);
}
else
{
	plan skip_all => 'Expensive data checksums test disabled';
}

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Bring the node back to "off" with no leftover table between
# scenarios.  The crash restarts already resolve an interrupted enable
# to "off", so only disable when something is left to disable.  Those
# restarts also clear any attached injection points.  The checkpoint
# bounds the WAL the next scenario's crash recovery has to replay.
sub reset_scenario
{
	my ($node) = @_;

	BAIL_OUT('node is not running after the previous scenario')
	  unless $node->is_alive;

	my $state = $node->safe_psql('postgres', 'SHOW data_checksums;');
	disable_data_checksums($node, wait => 'off') if $state ne 'off';
	$node->safe_psql('postgres', 'DROP TABLE IF EXISTS t;');
	$node->safe_psql('postgres', 'CHECKPOINT;');
	test_checksum_state($node, 'off');
}

my $node = PostgreSQL::Test::Cluster->new('crash_windows');
$node->init(no_data_checksums => 1);

# Scenario 1 needs the pages of "t" to reach disk without a full page
# image, hence full_page_writes and wal_log_hints off.  Scenario 2
# needs them to stay dirty in shared buffers until a checkpoint, hence
# no bgwriter and a large shared_buffers.  wal_level is pinned because
# init() writes "minimal" for a non-streaming node.  The rest merely
# tolerate the settings.
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
checkpoint_timeout = 1h
wal_level = replica
max_wal_size = 10GB
full_page_writes = off
wal_log_hints = off
shared_buffers = 512MB
bgwriter_lru_maxpages = 0
));
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');
$node->safe_psql('postgres', 'CREATE EXTENSION pg_buffercache;');

test_checksum_state($node, 'off');

# Scenario 1: a primary crashes inside SetDataChecksumsOn(), just before the
# forced checkpoint that flushes the rewritten pages, with crash recovery
# resuming from a checkpoint older than the transition.  The control file
# must still say "inprogress-on" there: replay does not adopt the state of
# the checkpoint record it resumes from, so an "on" written before the flush
# would stay in effect while replay reads pages whose rewrite never reached
# disk.  Here the pages went out through the rewriting worker's ring buffer,
# so only the control file state discriminates; see the next scenario for
# the case where they did not.
{
	$node->safe_psql('postgres',
		'CREATE TABLE t AS SELECT generate_series(1,10000) AS a;');

	# Establish the checkpoint that crash recovery will resume from.
	$node->safe_psql('postgres', 'CHECKPOINT;');

	# Dirty the pages of "t" without full page images, then push them out
	# to disk while checksums are still off.
	$node->safe_psql('postgres', 'UPDATE t SET a = a + 1;');
	my $relpath = $node->safe_psql('postgres',
		"SELECT pg_relation_filepath('t'::regclass);");
	my $evicted = $node->safe_psql('postgres',
		"SELECT pg_buffercache_evict_relation('t'::regclass);");
	note("evict_relation on t: $evicted, relpath $relpath");

	# Stop the enable right after the control file has been updated to
	# "on" but before the checkpoint that flushes the rewritten pages.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('datachecksums-on-before-checkpoint','wait');"
	);
	$node->safe_psql('postgres', 'SELECT pg_enable_data_checksums();');

	$node->poll_query_until('postgres',
		"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'datachecksums-on-before-checkpoint';"
	) or die 'timed out waiting for the injection point';

	my ($ctl) = run_command([ 'pg_controldata', $node->data_dir ]);
	my ($ctl_state) = $ctl =~ /Data page checksum version:\s+(\d+)/;
	note("control file data_checksum_version before the crash: $ctl_state");
	is($ctl_state, '3',
		'control file still says "inprogress-on" before the checkpoint');

	$node->stop('immediate');

	# Show the on-disk checksum field of the first page of "t".
	my $page;
	open(my $fh, '<', $node->data_dir . '/' . $relpath) or die $!;
	binmode $fh;
	my $len = read($fh, $page, 8192);
	isnt($len, undef, "Reading filehandle didn't fail");
	close($fh);
	my ($pd_checksum) = unpack('x8 v', $page);
	note("on-disk pd_checksum of t block 0: $pd_checksum");

	my $started = $node->start(fail_ok => 1);
	ok($started, 'primary restarts after crashing inside the online enable');

	my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile);
	unlike(
		$log,
		qr/page verification failed/,
		'no checksum verification failures while replaying');
	unlike(
		$log,
		qr/invalid page in block/,
		'no invalid pages while replaying');

	if ($started)
	{
		my ($rc, $stdout, $stderr) =
		  $node->psql('postgres', 'SELECT count(*) FROM t;');
		is($rc, 0, 'table readable after the crash restart')
		  or diag("stderr: $stderr");
	}
	else
	{
		my @lines = grep { /FATAL|PANIC|invalid page|verification failed/ }
		  split(/\n/, $log);
		diag("log tail:\n" . join("\n", @lines));
		fail('table readable after the crash restart');
	}
}
reset_scenario($node);

# Scenario 2: the same crash as above, but with the rewritten pages resident
# in shared buffers instead of gone out through the ring.  The rewriting
# worker reads through a BAS_VACUUM ring, which writes pages back as the
# ring recycles, but a page already resident in shared buffers is not read
# through the ring: ReadBufferExtended() hands back the existing buffer, and
# it stays dirty until a checkpoint.  The control file may therefore not
# say "on" before that checkpoint has run, or crash recovery would resume
# from a checkpoint older than the transition with verification already
# enabled, and replay records that read those still-unchecksummed pages.
# full_page_writes is off so that the records do not simply overwrite the
# pages with a full page image.
{
	$node->safe_psql('postgres',
		'CREATE TABLE t AS SELECT generate_series(1,100000) AS a;');
	my $relpath = $node->safe_psql('postgres',
		"SELECT pg_relation_filepath('t'::regclass);");

	# Establish the checkpoint that crash recovery will resume from, with the
	# pages of "t" written out while checksums are still off.
	$node->safe_psql('postgres', 'CHECKPOINT;');

	# Dirty those pages again without emitting full page images, and leave
	# them in shared buffers.  Replay of these records has to read the
	# pages from disk.
	$node->safe_psql('postgres', 'UPDATE t SET a = a + 1;');

	my $dirty_before = $node->safe_psql('postgres',
			"SELECT count(*) FROM pg_buffercache "
		  . "WHERE relfilenode = pg_relation_filenode('t'::regclass) "
		  . "AND relforknumber = 0 AND isdirty;");
	cmp_ok($dirty_before, '>', 0,
		'pages of t are resident and dirty before enabling checksums');

	# Hold the enabling right before the checkpoint that flushes the rewritten
	# pages.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('datachecksums-on-before-checkpoint','wait');"
	);

	enable_data_checksums($node);
	$node->wait_for_event('datachecksums launcher',
		'datachecksums-on-before-checkpoint');

	my ($ctl) = run_command([ 'pg_controldata', $node->data_dir ]);
	my ($ctl_state) = $ctl =~ /Data page checksum version:\s+(\d+)/;
	note("control file data_checksum_version before the crash: $ctl_state");
	is($ctl_state, '3',
		'control file still says "inprogress-on" before the checkpoint');

	# The rewritten pages must still be sitting dirty in shared buffers, or
	# the window this test is about does not exist.
	my $dirty_after = $node->safe_psql('postgres',
			"SELECT count(*) FROM pg_buffercache "
		  . "WHERE relfilenode = pg_relation_filenode('t'::regclass) "
		  . "AND relforknumber = 0 AND isdirty;");
	cmp_ok($dirty_after, '>', 0,
		'rewritten pages of t are still dirty before the checkpoint');

	$node->stop('immediate');

	# The on-disk copy is the one written before the transition, without a
	# checksum.
	my $page;
	open(my $fh, '<', $node->data_dir . '/' . $relpath) or die $!;
	binmode $fh;
	my $len = read($fh, $page, 8192);
	isnt($len, undef, "Reading filehandle didn't fail");
	close($fh);
	my ($pd_checksum) = unpack('x8 v', $page);
	note("on-disk pd_checksum of t block 0: $pd_checksum");
	is($pd_checksum, 0, 'block 0 of t on disk carries no checksum');

	my $started = $node->start(fail_ok => 1);
	ok($started, 'primary restarts after crashing inside the online enable');

	my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile);
	unlike(
		$log,
		qr/page verification failed/,
		'no checksum verification failures while replaying');
	unlike(
		$log,
		qr/invalid page in block/,
		'no invalid pages while replaying');

	if ($started)
	{
		my ($rc, $stdout, $stderr) =
		  $node->psql('postgres', 'SELECT count(*) FROM t;');
		is($rc, 0, 'table readable after the crash restart')
		  or diag("stderr: $stderr");
	}
	else
	{
		my @lines = grep { /FATAL|PANIC|invalid page|verification failed/ }
		  split(/\n/, $log);
		diag("log tail:\n" . join("\n", @lines));
		fail('table readable after the crash restart');
	}
}
reset_scenario($node);

# Scenario 3: a checkpoint that runs between the XLOG2_CHECKSUMS("on")
# record and the control file write at the end of SetDataChecksumsOn() must
# not make a crash throw the completed transition away.  SetDataChecksumsOn()
# writes the record, flips shared memory to "on", emits the barrier and only
# then requests the checkpoint that flushes the rewritten pages; the control
# file is written after that checkpoint returns.  A crash in that window is
# harmless only as long as recovery still starts before the record.  Any
# checkpoint completing in the window moves the redo point past the record,
# so recovery would never see it, would come up with the control file's
# "inprogress-on" and StartupXLOG() would demote that to "off", even though
# every page on disk carries a checksum by then.  CreateCheckPoint()
# therefore persists the state the checkpoint ran under, the same way
# CreateRestartPoint() does.  Here the window is made deterministic by
# holding the launcher at the datachecksums-on-before-checkpoint injection
# point and checkpointing from another session.
{
	$node->safe_psql('postgres',
		'CREATE TABLE t AS SELECT generate_series(1,10000) AS a;');
	my $relpath = $node->safe_psql('postgres',
		"SELECT pg_relation_filepath('t'::regclass);");

	# Hold the launcher after the record, the shared memory flip and the
	# barrier, but before the checkpoint SetDataChecksumsOn() requests
	# itself.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('datachecksums-on-before-checkpoint','wait');"
	);

	enable_data_checksums($node);
	$node->wait_for_event('datachecksums launcher',
		'datachecksums-on-before-checkpoint');

	# Every backend already sees "on" and writes checksums.
	test_checksum_state($node, 'on');

	# ... while the control file still says "inprogress-on".
	my ($ctl) = run_command([ 'pg_controldata', $node->data_dir ]);
	my ($before) = $ctl =~ /Data page checksum version:\s+(\d+)/;
	is($before, '3', 'control file says "inprogress-on" inside the window');

	# A concurrent checkpoint.  It flushes the rewritten pages and moves
	# the redo point past the XLOG2_CHECKSUMS("on") record, so it has to
	# record the "on" state in the control file as well.
	$node->safe_psql('postgres', 'CHECKPOINT;');

	($ctl) = run_command([ 'pg_controldata', $node->data_dir ]);
	my ($after) = $ctl =~ /Data page checksum version:\s+(\d+)/;
	is($after, '1',
		'the concurrent checkpoint records "on" in the control file');

	$node->stop('immediate');

	# The checkpoint flushed the rewritten pages, so they carry a checksum on
	# disk: the transition really did complete.
	my $page;
	open(my $fh, '<', $node->data_dir . '/' . $relpath) or die $!;
	binmode $fh;
	my $len = read($fh, $page, 8192);
	isnt($len, undef, "Reading filehandle didn't fail");
	close($fh);
	my ($pd_checksum) = unpack('x8 v', $page);
	note("on-disk pd_checksum of t block 0: $pd_checksum");
	isnt($pd_checksum, 0, 'block 0 of t on disk carries a checksum');

	my $log_offset = -s $node->logfile;
	$node->start;

	# The transition is complete on disk, so the cluster has to come back
	# "on".
	test_checksum_state($node, 'on');

	my $log =
	  PostgreSQL::Test::Utils::slurp_file($node->logfile, $log_offset);
	unlike(
		$log,
		qr/enabling data checksums was interrupted/,
		'the completed transition is not reported as interrupted');
}
reset_scenario($node);

# Scenario 4: a crash between the checkpoint an online enable requests and
# the control file write that follows it must not lose the transition.  The
# last steps of SetDataChecksumsOn() are
#
#	WAL record -> shmem -> barrier -> checkpoint -> persist
#
# The checkpoint is what makes "on" safe to persist, but it also moves the
# redo point above the XLOG2_CHECKSUMS record that carries the new state.
# Crash recovery started from that checkpoint therefore never replays the
# record, so without the state the checkpoint itself persists, a crash in
# the remaining window would bring the cluster back at "inprogress-on",
# which StartupXLOG() resolves to "off", discarding a transition whose
# pages are all on disk with a checksum.  The previous scenario exercises
# the same window through a checkpoint requested by another session; this
# one closes it from the other side: the transition's own checkpoint is the
# one that moves the redo point, so persisting the state from
# CreateCheckPoint() is what has to save it, not the write below the
# injection point.
{
	$node->safe_psql('postgres',
		'CREATE TABLE t AS SELECT generate_series(1,10000) AS a;');

	# Hold the launcher after the checkpoint that licenses "on" has
	# completed and before the state reaches the control file.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('datachecksums-on-after-checkpoint','wait');"
	);

	enable_data_checksums($node);
	$node->wait_for_event('datachecksums launcher',
		'datachecksums-on-after-checkpoint');

	# The transition is complete as far as the running cluster is concerned.
	test_checksum_state($node, 'on');

	# The checkpoint has already recorded it, so the pending write below the
	# injection point has nothing left to do.
	my ($ctl) = run_command([ 'pg_controldata', $node->data_dir ]);
	my ($version) = $ctl =~ /Data page checksum version:\s+(\d+)/;
	is($version, '1',
		'the requested checkpoint recorded "on" in the control file');

	# Crash before the control file write that follows the checkpoint.
	$node->stop('immediate');

	$node->start;

	# Every page on disk carries a checksum and the checkpoint that
	# flushed them completed, so the cluster has to come back verifying
	# them.
	test_checksum_state($node, 'on');

	is($node->safe_psql('postgres', 'SELECT count(*) FROM t;'),
		'10000', 'relation readable after the crash');

	my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile);
	unlike(
		$log,
		qr/enabling data checksums was interrupted/,
		'the completed transition is not reported as interrupted');

	# The data directory must be one the offline tools accept.
	$node->stop;
	$node->command_ok([ 'pg_checksums', '--check', '-D', $node->data_dir ],
		'pg_checksums accepts the data directory');

	# Leave the node running for the reset.
	$node->start;
}
reset_scenario($node);

# Scenario 5: a checkpoint racing SetDataChecksumsOn() between the insertion
# of the XLOG2_CHECKSUMS("on") record and the shared memory update must not
# insert an XLOG_CHECKPOINT_REDO record that follows the transition in WAL
# order while still carrying "inprogress-on".  Recovery resuming from such a
# redo point never replays the preceding transition record, comes up in
# "inprogress-on" and resolves the finished transition as interrupted.
# XLogChecksums() closes the window by inserting the record and publishing
# the new state under DataChecksumTransitionLock, which CreateCheckPoint()
# takes around sampling the state and inserting the redo record.  Here the
# launcher is held between the two steps at the
# datachecksums-on-before-publish injection point, and a concurrent
# CHECKPOINT has to block on the lock instead of completing inside the
# window.
{
	$node->safe_psql('postgres',
		'CREATE TABLE t AS SELECT generate_series(1,10000) AS a;');

	# The datachecksums-on-before-publish point fires inside a critical
	# section, where the wait machinery must not allocate.  Waiting once
	# at the datachecksums-enable-checksums-delay point, which the
	# launcher runs outside the critical section, initializes it; see
	# 050_redo_segment_missing.pl for the same recipe around
	# create-checkpoint-run.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('datachecksums-enable-checksums-delay','wait');"
	);

	# Hold the launcher after the XLOG2_CHECKSUMS("on") record is in WAL but
	# before the new state is published in shared memory.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('datachecksums-on-before-publish','wait');"
	);

	enable_data_checksums($node);
	$node->wait_for_event('datachecksums launcher',
		'datachecksums-enable-checksums-delay');
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('datachecksums-enable-checksums-delay');"
	);
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('datachecksums-enable-checksums-delay');"
	);

	$node->wait_for_event('datachecksums launcher',
		'datachecksums-on-before-publish');

	# The record is in WAL, the published state is still the old one.
	test_checksum_state($node, 'inprogress-on');

	# A concurrent checkpoint.  It must block on DataChecksumTransitionLock
	# before inserting its redo record rather than complete inside the window.
	my $checkpointer = IPC::Run::start(
		[
			'psql', '-XAtq',
			'-d', $node->connstr('postgres'),
			'-c', 'CHECKPOINT;'
		],
		'<' => '/dev/null',
		'>' => '/dev/null',
		'2>' => '/dev/null',
		IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default));

	ok( $node->poll_query_until(
			'postgres',
			"SELECT wait_event = 'DataChecksumTransition' "
			  . "FROM pg_stat_activity WHERE backend_type = 'checkpointer';"),
		'concurrent checkpoint blocks on DataChecksumTransitionLock');

	# Release the launcher; the checkpoint then samples the published "on" and
	# its redo record follows the transition record.
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('datachecksums-on-before-publish');");
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('datachecksums-on-before-publish');");

	$checkpointer->finish;

	# Crash while the launcher's own checkpoint may still be in flight.
	# Recovery resumes from the concurrent checkpoint's redo point, which
	# now lies above the transition record and carries "on".
	$node->stop('immediate');

	my $log_offset = -s $node->logfile;
	$node->start;

	# The transition completed, so the cluster has to come back "on".
	wait_for_checksum_state($node, 'on');

	my $log =
	  PostgreSQL::Test::Utils::slurp_file($node->logfile, $log_offset);
	unlike(
		$log,
		qr/enabling data checksums was interrupted/,
		'the completed transition is not reported as interrupted');

	$node->stop;
}

done_testing();
