# Copyright (c) 2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;


###
# Test io_method=worker
###
my $node_worker = create_node('worker');
$node_worker->start();

test_generic('worker', $node_worker);
SKIP:
{
	skip 'Injection points not supported by this build', 1
	  unless $ENV{enable_injection_points} eq 'yes';
	test_inject_worker('worker', $node_worker);
}

$node_worker->stop();


###
# Test io_method=io_uring
###

if (have_io_uring())
{
	my $node_uring = create_node('io_uring');
	$node_uring->start();
	test_generic('io_uring', $node_uring);
	$node_uring->stop();
}


###
# Test io_method=sync
###

my $node_sync = create_node('sync');

# just to have one test not use the default auto-tuning

$node_sync->append_conf(
	'postgresql.conf', qq(
io_max_concurrency=4
));

$node_sync->start();
test_generic('sync', $node_sync);
$node_sync->stop();

done_testing();


###
# Test Helpers
###

sub create_node
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $io_method = shift;

	my $node = PostgreSQL::Test::Cluster->new($io_method);

	# Want to test initdb for each IO method, otherwise we could just reuse
	# the cluster.
	#
	# Unfortunately Cluster::init() puts PG_TEST_INITDB_EXTRA_OPTS after the
	# options specified by ->extra, if somebody puts -c io_method=xyz in
	# PG_TEST_INITDB_EXTRA_OPTS it would break this test. Fix that up if we
	# detect it.
	local $ENV{PG_TEST_INITDB_EXTRA_OPTS} = $ENV{PG_TEST_INITDB_EXTRA_OPTS};
	if (defined $ENV{PG_TEST_INITDB_EXTRA_OPTS}
		&& $ENV{PG_TEST_INITDB_EXTRA_OPTS} =~ m/io_method=/)
	{
		$ENV{PG_TEST_INITDB_EXTRA_OPTS} .= " -c io_method=$io_method";
	}

	$node->init(extra => [ '-c', "io_method=$io_method" ]);

	$node->append_conf(
		'postgresql.conf', qq(
shared_preload_libraries=test_aio
log_min_messages = 'DEBUG3'
log_statement=all
log_error_verbosity=default
restart_after_crash=false
temp_buffers=100
));

	# Even though we used -c io_method=... above, if TEMP_CONFIG sets
	# io_method, it'd override the setting persisted at initdb time. While
	# using (and later verifying) the setting from initdb provides some
	# verification of having used the io_method during initdb, it's probably
	# not worth the complication of only appending if the variable is set in
	# in TEMP_CONFIG.
	$node->append_conf(
		'postgresql.conf', qq(
io_method=$io_method
));

	ok(1, "$io_method: initdb");

	return $node;
}

sub have_io_uring
{
	# To detect if io_uring is supported, we look at the error message for
	# assigning an invalid value to an enum GUC, which lists all the valid
	# options. We need to use -C to deal with running as administrator on
	# windows, the superuser check is omitted if -C is used.
	my ($stdout, $stderr) =
	  run_command [qw(postgres -C invalid -c io_method=invalid)];
	die "can't determine supported io_method values"
	  unless $stderr =~ m/Available values: ([^\.]+)\./;
	my $methods = $1;
	note "supported io_method values are: $methods";

	return ($methods =~ m/io_uring/) ? 1 : 0;
}

sub psql_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my $io_method = shift;
	my $psql = shift;
	my $name = shift;
	my $sql = shift;
	my $expected_stdout = shift;
	my $expected_stderr = shift;
	my ($cmdret, $output);

	($output, $cmdret) = $psql->query($sql);

	like($output, $expected_stdout, "$io_method: $name: expected stdout");
	like($psql->{stderr}, $expected_stderr,
		"$io_method: $name: expected stderr");
	$psql->{stderr} = '';

	return $output;
}

sub query_wait_block
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my $io_method = shift;
	my $node = shift;
	my $psql = shift;
	my $name = shift;
	my $sql = shift;
	my $waitfor = shift;

	my $pid = $psql->query_safe('SELECT pg_backend_pid()');

	$psql->{stdin} .= qq($sql;\n);
	$psql->{run}->pump_nb();
	ok(1, "$io_method: $name: issued sql");

	$node->poll_query_until('postgres',
		qq(SELECT wait_event FROM pg_stat_activity WHERE pid = $pid),
		$waitfor);
	ok(1, "$io_method: $name: observed $waitfor wait event");
}

# Returns count of checksum failures for the specified database or for shared
# relations, if $datname is undefined.
sub checksum_failures
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my $psql = shift;
	my $datname = shift;
	my $checksum_count;
	my $checksum_last_failure;

	if (defined $datname)
	{
		$checksum_count = $psql->query_safe(
			qq(
SELECT checksum_failures FROM pg_stat_database WHERE datname = '$datname';
));
		$checksum_last_failure = $psql->query_safe(
			qq(
SELECT checksum_last_failure FROM pg_stat_database WHERE datname = '$datname';
));
	}
	else
	{
		$checksum_count = $psql->query_safe(
			qq(
SELECT checksum_failures FROM pg_stat_database WHERE datname IS NULL;
));
		$checksum_last_failure = $psql->query_safe(
			qq(
SELECT checksum_last_failure FROM pg_stat_database WHERE datname IS NULL;
));
	}

	return $checksum_count, $checksum_last_failure;
}

###
# Sub-tests
###

# Sanity checks for the IO handle API
sub test_handle
{
	my $io_method = shift;
	my $node = shift;

	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	# leak warning: implicit xact
	psql_like(
		$io_method,
		$psql,
		"handle_get() leak in implicit xact",
		qq(SELECT handle_get()),
		qr/^$/,
		qr/leaked AIO handle/,
		"$io_method: leaky handle_get() warns");

	# leak warning: explicit xact
	psql_like(
		$io_method, $psql,
		"handle_get() leak in explicit xact",
		qq(BEGIN; SELECT handle_get(); COMMIT),
		qr/^$/, qr/leaked AIO handle/);


	# leak warning: explicit xact, rollback
	psql_like(
		$io_method,
		$psql,
		"handle_get() leak in explicit xact, rollback",
		qq(BEGIN; SELECT handle_get(); ROLLBACK;),
		qr/^$/,
		qr/leaked AIO handle/);

	# leak warning: subtrans
	psql_like(
		$io_method,
		$psql,
		"handle_get() leak in subxact",
		qq(BEGIN; SAVEPOINT foo; SELECT handle_get(); COMMIT;),
		qr/^$/,
		qr/leaked AIO handle/);

	# leak warning + error: released in different command (thus resowner)
	psql_like(
		$io_method,
		$psql,
		"handle_release() in different command",
		qq(BEGIN; SELECT handle_get(); SELECT handle_release_last(); COMMIT;),
		qr/^$/,
		qr/leaked AIO handle.*release in unexpected state/ms);

	# no leak, release in same command
	psql_like(
		$io_method,
		$psql,
		"handle_release() in same command",
		qq(BEGIN; SELECT handle_get() UNION ALL SELECT handle_release_last(); COMMIT;),
		qr/^$/,
		qr/^$/);

	# normal handle use
	psql_like($io_method, $psql, "handle_get_release()",
		qq(SELECT handle_get_release()),
		qr/^$/, qr/^$/);

	# should error out, API violation
	psql_like(
		$io_method,
		$psql,
		"handle_get_twice()",
		qq(SELECT handle_get_twice()),
		qr/^$/,
		qr/ERROR:  API violation: Only one IO can be handed out$/);

	# recover after error in implicit xact
	psql_like(
		$io_method,
		$psql,
		"handle error recovery in implicit xact",
		qq(SELECT handle_get_and_error(); SELECT 'ok', handle_get_release()),
		qr/^|ok$/,
		qr/ERROR.*as you command/);

	# recover after error in implicit xact
	psql_like(
		$io_method,
		$psql,
		"handle error recovery in explicit xact",
		qq(BEGIN; SELECT handle_get_and_error(); SELECT handle_get_release(), 'ok'; COMMIT;),
		qr/^|ok$/,
		qr/ERROR.*as you command/);

	# recover after error in subtrans
	psql_like(
		$io_method,
		$psql,
		"handle error recovery in explicit subxact",
		qq(BEGIN; SAVEPOINT foo; SELECT handle_get_and_error(); ROLLBACK TO SAVEPOINT foo; SELECT handle_get_release(); ROLLBACK;),
		qr/^|ok$/,
		qr/ERROR.*as you command/);

	$psql->quit();
}

# Sanity checks for the batchmode API
sub test_batchmode
{
	my $io_method = shift;
	my $node = shift;

	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	# In a build with RELCACHE_FORCE_RELEASE and CATCACHE_FORCE_RELEASE, just
	# using SELECT batch_start() causes spurious test failures, because the
	# lookup of the type information when printing the result tuple also
	# starts a batch. The easiest way around is to not print a result tuple.
	my $batch_start_sql = qq(SELECT WHERE batch_start() IS NULL);

	# leak warning & recovery: implicit xact
	psql_like(
		$io_method,
		$psql,
		"batch_start() leak & cleanup in implicit xact",
		$batch_start_sql,
		qr/^$/,
		qr/open AIO batch at end/,
		"$io_method: leaky batch_start() warns");

	# leak warning & recovery: explicit xact
	psql_like(
		$io_method,
		$psql,
		"batch_start() leak & cleanup in explicit xact",
		qq(BEGIN; $batch_start_sql; COMMIT;),
		qr/^$/,
		qr/open AIO batch at end/,
		"$io_method: leaky batch_start() warns");


	# leak warning & recovery: explicit xact, rollback
	#
	# XXX: This doesn't fail right now, due to not getting a chance to do
	# something at transaction command commit. That's not a correctness issue,
	# it just means it's a bit harder to find buggy code.
	#psql_like($io_method, $psql,
	#		  "batch_start() leak & cleanup after abort",
	#		  qq(BEGIN; $batch_start_sql; ROLLBACK;),
	#		  qr/^$/,
	#		  qr/open AIO batch at end/, "$io_method: leaky batch_start() warns");

	# no warning, batch closed in same command
	psql_like(
		$io_method,
		$psql,
		"batch_start(), batch_end() works",
		qq($batch_start_sql UNION ALL SELECT WHERE batch_end() IS NULL),
		qr/^$/,
		qr/^$/,
		"$io_method: batch_start(), batch_end()");

	$psql->quit();
}

# Test that simple cases of invalid pages are reported
sub test_io_error
{
	my $io_method = shift;
	my $node = shift;
	my ($ret, $output);

	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	$psql->query_safe(
		qq(
CREATE TEMPORARY TABLE tmp_corr(data int not null);
INSERT INTO tmp_corr SELECT generate_series(1, 10000);
SELECT modify_rel_block('tmp_corr', 1, corrupt_header=>true);
));

	foreach my $tblname (qw(tbl_corr tmp_corr))
	{
		my $invalid_page_re =
		  $tblname eq 'tbl_corr'
		  ? qr/invalid page in block 1 of relation base\/\d+\/\d+/
		  : qr/invalid page in block 1 of relation base\/\d+\/t\d+_\d+/;

		# verify the error is reported in custom C code
		psql_like(
			$io_method,
			$psql,
			"read_rel_block_ll() of $tblname page",
			qq(SELECT read_rel_block_ll('$tblname', 1)),
			qr/^$/,
			$invalid_page_re);

		# verify the error is reported for bufmgr reads, seq scan
		psql_like(
			$io_method, $psql,
			"sequential scan of $tblname block fails",
			qq(SELECT count(*) FROM $tblname),
			qr/^$/, $invalid_page_re);

		# verify the error is reported for bufmgr reads, tid scan
		psql_like(
			$io_method,
			$psql,
			"tid scan of $tblname block fails",
			qq(SELECT count(*) FROM $tblname WHERE ctid = '(1, 1)'),
			qr/^$/,
			$invalid_page_re);
	}

	$psql->quit();
}

# Test interplay between StartBufferIO and TerminateBufferIO
sub test_startwait_io
{
	my $io_method = shift;
	my $node = shift;
	my ($ret, $output);

	my $psql_a = $node->background_psql('postgres', on_error_stop => 0);
	my $psql_b = $node->background_psql('postgres', on_error_stop => 0);


	### Verify behavior for normal tables

	# create a buffer we can play around with
	my $buf_id = psql_like(
		$io_method, $psql_a,
		"creation of toy buffer succeeds",
		qq(SELECT buffer_create_toy('tbl_ok', 1)),
		qr/^\d+$/, qr/^$/);

	# check that one backend can perform StartBufferIO
	psql_like(
		$io_method,
		$psql_a,
		"first StartBufferIO",
		qq(SELECT buffer_call_start_io($buf_id, for_input=>true, nowait=>false);),
		qr/^t$/,
		qr/^$/);

	# but not twice on the same buffer (non-waiting)
	psql_like(
		$io_method,
		$psql_a,
		"second StartBufferIO fails, same session",
		qq(SELECT buffer_call_start_io($buf_id, for_input=>true, nowait=>true);),
		qr/^f$/,
		qr/^$/);
	psql_like(
		$io_method,
		$psql_b,
		"second StartBufferIO fails, other session",
		qq(SELECT buffer_call_start_io($buf_id, for_input=>true, nowait=>true);),
		qr/^f$/,
		qr/^$/);

	# start io in a different session, will block
	query_wait_block(
		$io_method,
		$node,
		$psql_b,
		"blocking start buffer io",
		qq(SELECT buffer_call_start_io($buf_id, for_input=>true, nowait=>false);),
		"BufferIo");

	# Terminate the IO, without marking it as success, this should trigger the
	# waiting session to be able to start the io
	psql_like(
		$io_method,
		$psql_a,
		"blocking start buffer io, terminating io, not valid",
		qq(SELECT buffer_call_terminate_io($buf_id, for_input=>true, succeed=>false, io_error=>false, release_aio=>false)),
		qr/^$/,
		qr/^$/);


	# Because the IO was terminated, but not marked as valid, second session should get the right to start io
	pump_until($psql_b->{run}, $psql_b->{timeout}, \$psql_b->{stdout}, qr/t/);
	ok(1, "$io_method: blocking start buffer io, can start io");

	# terminate the IO again
	$psql_b->query_safe(
		qq(SELECT buffer_call_terminate_io($buf_id, for_input=>true, succeed=>false, io_error=>false, release_aio=>false);)
	);


	# same as the above scenario, but mark IO as having succeeded
	psql_like(
		$io_method,
		$psql_a,
		"blocking buffer io w/ success: first start buffer io",
		qq(SELECT buffer_call_start_io($buf_id, for_input=>true, nowait=>false);),
		qr/^t$/,
		qr/^$/);

	# start io in a different session, will block
	query_wait_block(
		$io_method,
		$node,
		$psql_b,
		"blocking start buffer io",
		qq(SELECT buffer_call_start_io($buf_id, for_input=>true, nowait=>false);),
		"BufferIo");

	# Terminate the IO, marking it as success
	psql_like(
		$io_method,
		$psql_a,
		"blocking start buffer io, terminating io, valid",
		qq(SELECT buffer_call_terminate_io($buf_id, for_input=>true, succeed=>true, io_error=>false, release_aio=>false)),
		qr/^$/,
		qr/^$/);

	# Because the IO was terminated, and marked as valid, second session should complete but not need io
	pump_until($psql_b->{run}, $psql_b->{timeout}, \$psql_b->{stdout}, qr/f/);
	ok(1, "$io_method: blocking start buffer io, no need to start io");

	# buffer is valid now, make it invalid again
	$psql_a->query_safe(qq(SELECT buffer_create_toy('tbl_ok', 1);));


	### Verify behavior for temporary tables

	# Can't unfortunately share the code with the normal table case, there are
	# too many behavioral differences.

	# create a buffer we can play around with
	$psql_a->query_safe(
		qq(
CREATE TEMPORARY TABLE tmp_ok(data int not null);
INSERT INTO tmp_ok SELECT generate_series(1, 10000);
));
	$buf_id = $psql_a->query_safe(qq(SELECT buffer_create_toy('tmp_ok', 3);));

	# check that one backend can perform StartLocalBufferIO
	psql_like(
		$io_method,
		$psql_a,
		"first StartLocalBufferIO",
		qq(SELECT buffer_call_start_io($buf_id, for_input=>true, nowait=>true);),
		qr/^t$/,
		qr/^$/);

	# Because local buffers don't use IO_IN_PROGRESS, a second StartLocalBufferIO
	# succeeds as well. This test mostly serves as a documentation of that
	# fact. If we had actually started IO, it'd be different.
	psql_like(
		$io_method,
		$psql_a,
		"second StartLocalBufferIO succeeds, same session",
		qq(SELECT buffer_call_start_io($buf_id, for_input=>true, nowait=>true);),
		qr/^t$/,
		qr/^$/);

	# Terminate the IO again, without marking it as a success
	$psql_a->query_safe(
		qq(SELECT buffer_call_terminate_io($buf_id, for_input=>true, succeed=>false, io_error=>false, release_aio=>false);)
	);
	psql_like(
		$io_method,
		$psql_a,
		"StartLocalBufferIO after not marking valid succeeds, same session",
		qq(SELECT buffer_call_start_io($buf_id, for_input=>true, nowait=>true);),
		qr/^t$/,
		qr/^$/);

	# Terminate the IO again, marking it as a success
	$psql_a->query_safe(
		qq(SELECT buffer_call_terminate_io($buf_id, for_input=>true, succeed=>true, io_error=>false, release_aio=>false);)
	);

	# Now another StartLocalBufferIO should fail, this time because the buffer
	# is already valid.
	psql_like(
		$io_method,
		$psql_a,
		"StartLocalBufferIO after marking valid fails",
		qq(SELECT buffer_call_start_io($buf_id, for_input=>true, nowait=>false);),
		qr/^f$/,
		qr/^$/);

	$psql_a->quit();
	$psql_b->quit();
}

# Test that if the backend issuing a read doesn't wait for the IO's
# completion, another backend can complete the IO
sub test_complete_foreign
{
	my $io_method = shift;
	my $node = shift;
	my ($ret, $output);

	my $psql_a = $node->background_psql('postgres', on_error_stop => 0);
	my $psql_b = $node->background_psql('postgres', on_error_stop => 0);

	# Issue IO without waiting for completion, then sleep
	$psql_a->query_safe(
		qq(SELECT read_rel_block_ll('tbl_ok', 1, wait_complete=>false);));

	# Check that another backend can read the relevant block
	psql_like(
		$io_method,
		$psql_b,
		"completing read started by sleeping backend",
		qq(SELECT count(*) FROM tbl_ok WHERE ctid = '(1,1)' LIMIT 1),
		qr/^1$/,
		qr/^$/);

	# Issue IO without waiting for completion, then exit.
	$psql_a->query_safe(
		qq(SELECT read_rel_block_ll('tbl_ok', 1, wait_complete=>false);));
	$psql_a->reconnect_and_clear();

	# Check that another backend can read the relevant block. This verifies
	# that the exiting backend left the AIO in a sane state.
	psql_like(
		$io_method,
		$psql_b,
		"read buffer started by exited backend",
		qq(SELECT count(*) FROM tbl_ok WHERE ctid = '(1,1)' LIMIT 1),
		qr/^1$/,
		qr/^$/);

	# Read a tbl_corr block, then sleep. The other session will retry the IO
	# and also fail. The easiest thing to verify that seems to be to check
	# that both are in the log.
	my $log_location = -s $node->logfile;
	$psql_a->query_safe(
		qq(SELECT read_rel_block_ll('tbl_corr', 1, wait_complete=>false);));

	psql_like(
		$io_method,
		$psql_b,
		"completing read of tbl_corr block started by other backend",
		qq(SELECT count(*) FROM tbl_corr WHERE ctid = '(1,1)' LIMIT 1),
		qr/^$/,
		qr/invalid page in block/);

	# The log message issued for the read_rel_block_ll() should be logged as a LOG
	$node->wait_for_log(qr/LOG[^\n]+invalid page in/, $log_location);
	ok(1,
		"$io_method: completing read of tbl_corr block started by other backend: LOG message for background read"
	);

	# But for the SELECT, it should be an ERROR
	$log_location =
	  $node->wait_for_log(qr/ERROR[^\n]+invalid page in/, $log_location);
	ok(1,
		"$io_method: completing read of tbl_corr block started by other backend: ERROR message for foreground read"
	);

	$psql_a->quit();
	$psql_b->quit();
}

# Test that we deal correctly with FDs being closed while IO is in progress
sub test_close_fd
{
	my $io_method = shift;
	my $node = shift;
	my ($ret, $output);

	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	psql_like(
		$io_method,
		$psql,
		"close all FDs after read, waiting for results",
		qq(
			SELECT read_rel_block_ll('tbl_ok', 1,
				wait_complete=>true,
				batchmode_enter=>true,
				smgrreleaseall=>true,
				batchmode_exit=>true
			);),
		qr/^$/,
		qr/^$/);

	psql_like(
		$io_method,
		$psql,
		"close all FDs after read, no waiting",
		qq(
			SELECT read_rel_block_ll('tbl_ok', 1,
				wait_complete=>false,
				batchmode_enter=>true,
				smgrreleaseall=>true,
				batchmode_exit=>true
			);),
		qr/^$/,
		qr/^$/);

	# Check that another backend can read the relevant block
	psql_like(
		$io_method,
		$psql,
		"close all FDs after read, no waiting, query works",
		qq(SELECT count(*) FROM tbl_ok WHERE ctid = '(1,1)' LIMIT 1),
		qr/^1$/,
		qr/^$/);

	$psql->quit();
}

# Tests using injection points. Mostly to exercise hard IO errors that are
# hard to trigger without using injection points.
sub test_inject
{
	my $io_method = shift;
	my $node = shift;
	my ($ret, $output);

	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	# injected what we'd expect
	$psql->query_safe(qq(SELECT inj_io_short_read_attach(8192);));
	$psql->query_safe(qq(SELECT invalidate_rel_block('tbl_ok', 2);));
	psql_like(
		$io_method, $psql,
		"injection point not triggering failure",
		qq(SELECT count(*) FROM tbl_ok WHERE ctid = '(2, 1)'),
		qr/^1$/, qr/^$/);

	# injected a read shorter than a single block, expecting error
	$psql->query_safe(qq(SELECT inj_io_short_read_attach(17);));
	$psql->query_safe(qq(SELECT invalidate_rel_block('tbl_ok', 2);));
	psql_like(
		$io_method,
		$psql,
		"single block short read fails",
		qq(SELECT count(*) FROM tbl_ok WHERE ctid = '(2, 1)'),
		qr/^$/,
		qr/ERROR:.*could not read blocks 2\.\.2 in file "base\/.*": read only 0 of 8192 bytes/
	);

	# shorten multi-block read to a single block, should retry
	my $inval_query = qq(SELECT invalidate_rel_block('tbl_ok', 0);
SELECT invalidate_rel_block('tbl_ok', 1);
SELECT invalidate_rel_block('tbl_ok', 2);
SELECT invalidate_rel_block('tbl_ok', 3);
/* gap */
SELECT invalidate_rel_block('tbl_ok', 5);
SELECT invalidate_rel_block('tbl_ok', 6);
SELECT invalidate_rel_block('tbl_ok', 7);
SELECT invalidate_rel_block('tbl_ok', 8););

	$psql->query_safe($inval_query);
	$psql->query_safe(qq(SELECT inj_io_short_read_attach(8192);));
	psql_like(
		$io_method, $psql,
		"multi block short read (1 block) is retried",
		qq(SELECT count(*) FROM tbl_ok),
		qr/^10000$/, qr/^$/);

	# shorten multi-block read to two blocks, should retry
	$psql->query_safe($inval_query);
	$psql->query_safe(qq(SELECT inj_io_short_read_attach(8192*2);));

	psql_like(
		$io_method, $psql,
		"multi block short read (2 blocks) is retried",
		qq(SELECT count(*) FROM tbl_ok),
		qr/^10000$/, qr/^$/);

	# verify that page verification errors are detected even as part of a
	# shortened multi-block read (tbl_corr, block 1 is corrupted)
	$psql->query_safe(
		qq(
SELECT invalidate_rel_block('tbl_corr', 0);
SELECT invalidate_rel_block('tbl_corr', 1);
SELECT invalidate_rel_block('tbl_corr', 2);
SELECT inj_io_short_read_attach(8192);
	));

	psql_like(
		$io_method,
		$psql,
		"shortened multi-block read detects invalid page",
		qq(SELECT count(*) FROM tbl_corr WHERE ctid < '(2, 1)'),
		qr/^$/,
		qr/ERROR:.*invalid page in block 1 of relation base\/.*/);

	# trigger a hard error, should error out
	$psql->query_safe(
		qq(
SELECT inj_io_short_read_attach(-errno_from_string('EIO'));
SELECT invalidate_rel_block('tbl_ok', 2);
	));

	psql_like(
		$io_method,
		$psql,
		"first hard IO error is reported",
		qq(SELECT count(*) FROM tbl_ok),
		qr/^$/,
		qr!ERROR:.*could not read blocks 2\.\.2 in file "base/.*": (?:I/O|Input/output) error!
	);

	psql_like(
		$io_method,
		$psql,
		"second hard IO error is reported",
		qq(SELECT count(*) FROM tbl_ok),
		qr/^$/,
		qr!ERROR:.*could not read blocks 2\.\.2 in file "base/.*": (?:I/O|Input/output) error!
	);

	$psql->query_safe(qq(SELECT inj_io_short_read_detach()));

	# now the IO should be ok.
	psql_like(
		$io_method, $psql,
		"recovers after hard error",
		qq(SELECT count(*) FROM tbl_ok),
		qr/^10000$/, qr/^$/);

	# trigger a different hard error, should error out
	$psql->query_safe(
		qq(
SELECT inj_io_short_read_attach(-errno_from_string('EROFS'));
SELECT invalidate_rel_block('tbl_ok', 2);
	));
	psql_like(
		$io_method,
		$psql,
		"different hard IO error is reported",
		qq(SELECT count(*) FROM tbl_ok),
		qr/^$/,
		qr/ERROR:.*could not read blocks 2\.\.2 in file \"base\/.*\": Read-only file system/
	);
	$psql->query_safe(qq(SELECT inj_io_short_read_detach()));

	$psql->quit();
}

# Tests using injection points, only for io_method=worker.
#
# io_method=worker has the special case of needing to reopen files. That can
# in theory fail, because the file could be gone. That's a hard path to test
# for real, so we use an injection point to trigger it.
sub test_inject_worker
{
	my $io_method = shift;
	my $node = shift;
	my ($ret, $output);

	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	# trigger a failure to reopen, should error out, but should recover
	$psql->query_safe(
		qq(
SELECT inj_io_reopen_attach();
SELECT invalidate_rel_block('tbl_ok', 1);
	));

	psql_like(
		$io_method,
		$psql,
		"failure to open: detected",
		qq(SELECT count(*) FROM tbl_ok),
		qr/^$/,
		qr/ERROR:.*could not read blocks 1\.\.1 in file "base\/.*": No such file or directory/
	);

	$psql->query_safe(qq(SELECT inj_io_reopen_detach();));

	# check that we indeed recover
	psql_like(
		$io_method, $psql,
		"failure to open: recovers",
		qq(SELECT count(*) FROM tbl_ok),
		qr/^10000$/, qr/^$/);

	$psql->quit();
}

# Verify that we handle a relation getting removed (due to a rollback or a
# DROP TABLE) while IO is ongoing for that table.
sub test_invalidate
{
	my $io_method = shift;
	my $node = shift;

	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	foreach my $persistency (qw(normal unlogged temporary))
	{
		my $sql_persistency = $persistency eq 'normal' ? '' : $persistency;
		my $tblname = $persistency . '_transactional';

		my $create_sql = qq(
CREATE $sql_persistency TABLE $tblname (id int not null, data text not null) WITH (AUTOVACUUM_ENABLED = false);
INSERT INTO $tblname(id, data) SELECT generate_series(1, 10000) as id, repeat('a', 200);
);

		# Verify that outstanding read IO does not cause problems with
		# AbortTransaction -> smgrDoPendingDeletes -> smgrdounlinkall -> ...
		# -> Invalidate[Local]Buffer.
		$psql->query_safe("BEGIN; $create_sql;");
		$psql->query_safe(
			qq(
SELECT read_rel_block_ll('$tblname', 1, wait_complete=>false);
));
		psql_like(
			$io_method,
			$psql,
			"rollback of newly created $persistency table with outstanding IO",
			qq(ROLLBACK),
			qr/^$/,
			qr/^$/);

		# Verify that outstanding read IO does not cause problems with
		# CommitTransaction -> smgrDoPendingDeletes -> smgrdounlinkall -> ...
		# -> Invalidate[Local]Buffer.
		$psql->query_safe("BEGIN; $create_sql; COMMIT;");
		$psql->query_safe(
			qq(
BEGIN;
SELECT read_rel_block_ll('$tblname', 1, wait_complete=>false);
));

		psql_like(
			$io_method, $psql,
			"drop $persistency table with outstanding IO",
			qq(DROP TABLE $tblname),
			qr/^$/, qr/^$/);

		psql_like($io_method, $psql,
			"commit after drop $persistency table with outstanding IO",
			qq(COMMIT), qr/^$/, qr/^$/);
	}

	$psql->quit();
}

# Test behavior related to ZERO_ON_ERROR and zero_damaged_pages
sub test_zero
{
	my $io_method = shift;
	my $node = shift;

	my $psql_a = $node->background_psql('postgres', on_error_stop => 0);
	my $psql_b = $node->background_psql('postgres', on_error_stop => 0);

	foreach my $persistency (qw(normal temporary))
	{
		my $sql_persistency = $persistency eq 'normal' ? '' : $persistency;

		$psql_a->query_safe(
			qq(
CREATE $sql_persistency TABLE tbl_zero(id int) WITH (AUTOVACUUM_ENABLED = false);
INSERT INTO tbl_zero SELECT generate_series(1, 10000);
));

		$psql_a->query_safe(
			qq(
SELECT modify_rel_block('tbl_zero', 0, corrupt_header=>true);
));

		# Check that page validity errors are detected
		psql_like(
			$io_method,
			$psql_a,
			"$persistency: test reading of invalid block 0",
			qq(
SELECT read_rel_block_ll('tbl_zero', 0, zero_on_error=>false)),
			qr/^$/,
			qr/^psql:<stdin>:\d+: ERROR:  invalid page in block 0 of relation base\/.*\/.*$/
		);

		# Check that page validity errors are zeroed
		psql_like(
			$io_method,
			$psql_a,
			"$persistency: test zeroing of invalid block 0",
			qq(
SELECT read_rel_block_ll('tbl_zero', 0, zero_on_error=>true)),
			qr/^$/,
			qr/^psql:<stdin>:\d+: WARNING:  invalid page in block 0 of relation base\/.*\/.*; zeroing out page$/
		);

		# And that once the corruption is fixed, we can read again
		$psql_a->query(
			qq(
SELECT modify_rel_block('tbl_zero', 0, zero=>true);
));
		$psql_a->{stderr} = '';

		psql_like(
			$io_method,
			$psql_a,
			"$persistency: test re-read of block 0",
			qq(
SELECT read_rel_block_ll('tbl_zero', 0, zero_on_error=>false)),
			qr/^$/,
			qr/^$/);

		# Check a page validity error in another block, to ensure we report
		# the correct block number
		$psql_a->query_safe(
			qq(
SELECT modify_rel_block('tbl_zero', 3, corrupt_header=>true);
));
		psql_like(
			$io_method,
			$psql_a,
			"$persistency: test zeroing of invalid block 3",
			qq(SELECT read_rel_block_ll('tbl_zero', 3, zero_on_error=>true);),
			qr/^$/,
			qr/^psql:<stdin>:\d+: WARNING:  invalid page in block 3 of relation base\/.*\/.*; zeroing out page$/
		);


		# Check one read reporting multiple invalid blocks
		$psql_a->query_safe(
			qq(
SELECT modify_rel_block('tbl_zero', 2, corrupt_header=>true);
SELECT modify_rel_block('tbl_zero', 3, corrupt_header=>true);
));
		# First test error
		psql_like(
			$io_method,
			$psql_a,
			"$persistency: test reading of invalid block 2,3 in larger read",
			qq(SELECT read_rel_block_ll('tbl_zero', 1, nblocks=>4, zero_on_error=>false)),
			qr/^$/,
			qr/^psql:<stdin>:\d+: ERROR:  2 invalid pages among blocks 1..4 of relation base\/.*\/.*\nDETAIL:  Block 2 held first invalid page\.\nHINT:[^\n]+$/
		);

		# Then test zeroing via ZERO_ON_ERROR flag
		psql_like(
			$io_method,
			$psql_a,
			"$persistency: test zeroing of invalid block 2,3 in larger read, ZERO_ON_ERROR",
			qq(SELECT read_rel_block_ll('tbl_zero', 1, nblocks=>4, zero_on_error=>true)),
			qr/^$/,
			qr/^psql:<stdin>:\d+: WARNING:  zeroing out 2 invalid pages among blocks 1..4 of relation base\/.*\/.*\nDETAIL:  Block 2 held first zeroed page\.\nHINT:[^\n]+$/
		);

		# Then test zeroing via zero_damaged_pages
		psql_like(
			$io_method,
			$psql_a,
			"$persistency: test zeroing of invalid block 2,3 in larger read, zero_damaged_pages",
			qq(
BEGIN;
SET LOCAL zero_damaged_pages = true;
SELECT read_rel_block_ll('tbl_zero', 1, nblocks=>4, zero_on_error=>false)
COMMIT;
),
			qr/^$/,
			qr/^psql:<stdin>:\d+: WARNING:  zeroing out 2 invalid pages among blocks 1..4 of relation base\/.*\/.*\nDETAIL:  Block 2 held first zeroed page\.\nHINT:[^\n]+$/
		);

		$psql_a->query_safe(qq(COMMIT));


		# Verify that bufmgr.c IO detects page validity errors
		$psql_a->query(
			qq(
SELECT invalidate_rel_block('tbl_zero', g.i)
FROM generate_series(0, 15) g(i);
SELECT modify_rel_block('tbl_zero', 3, zero=>true);
));
		$psql_a->{stderr} = '';

		psql_like(
			$io_method,
			$psql_a,
			"$persistency: verify reading zero_damaged_pages=off",
			qq(
SELECT count(*) FROM tbl_zero),
			qr/^$/,
			qr/^psql:<stdin>:\d+: ERROR:  invalid page in block 2 of relation base\/.*\/.*$/
		);

		# Verify that bufmgr.c IO zeroes out pages with page validity errors
		psql_like(
			$io_method,
			$psql_a,
			"$persistency: verify zero_damaged_pages=on",
			qq(
BEGIN;
SET LOCAL zero_damaged_pages = true;
SELECT count(*) FROM tbl_zero;
COMMIT;
),
			qr/^\d+$/,
			qr/^psql:<stdin>:\d+: WARNING:  invalid page in block 2 of relation base\/.*\/.*$/
		);

		# Check that warnings/errors about page validity in an IO started by
		# session A that session B might complete aren't logged visibly to
		# session B.
		#
		# This will only ever trigger for io_method's like io_uring, that can
		# complete IO's in a client backend. But it doesn't seem worth
		# restricting to that.
		#
		# This requires cross-session access to the same relation, hence the
		# restriction to non-temporary table.
		if ($sql_persistency ne 'temporary')
		{
			# Create a corruption and then read the block without waiting for
			# completion.
			$psql_a->query(
				qq(
SELECT modify_rel_block('tbl_zero', 1, corrupt_header=>true);
SELECT read_rel_block_ll('tbl_zero', 1, wait_complete=>false, zero_on_error=>true)
));

			psql_like(
				$io_method,
				$psql_b,
				"$persistency: test completing read by other session doesn't generate warning",
				qq(SELECT count(*) > 0 FROM tbl_zero;),
				qr/^t$/,
				qr/^$/);
		}

		# Clean up
		$psql_a->query_safe(
			qq(
DROP TABLE tbl_zero;
));
	}

	$psql_a->{stderr} = '';

	$psql_a->quit();
	$psql_b->quit();
}

# Test that we detect checksum failures and report them
sub test_checksum
{
	my $io_method = shift;
	my $node = shift;

	my $psql_a = $node->background_psql('postgres', on_error_stop => 0);

	$psql_a->query_safe(
		qq(
CREATE TABLE tbl_normal(id int) WITH (AUTOVACUUM_ENABLED = false);
INSERT INTO tbl_normal SELECT generate_series(1, 5000);
SELECT modify_rel_block('tbl_normal', 3, corrupt_checksum=>true);

CREATE TEMPORARY TABLE tbl_temp(id int) WITH (AUTOVACUUM_ENABLED = false);
INSERT INTO tbl_temp SELECT generate_series(1, 5000);
SELECT modify_rel_block('tbl_temp', 3, corrupt_checksum=>true);
SELECT modify_rel_block('tbl_temp', 4, corrupt_checksum=>true);
));

	# To be able to test checksum failures on shared rels we need a shared rel
	# with invalid pages - which is a bit scary. pg_shseclabel seems like a
	# good bet, as it's not accessed in a default configuration.
	$psql_a->query_safe(
		qq(
SELECT grow_rel('pg_shseclabel', 4);
SELECT modify_rel_block('pg_shseclabel', 2, corrupt_checksum=>true);
SELECT modify_rel_block('pg_shseclabel', 3, corrupt_checksum=>true);
));


	# Check that page validity errors are detected, checksums stats increase, normal rel
	my ($cs_count_before, $cs_ts_before) =
	  checksum_failures($psql_a, 'postgres');
	psql_like(
		$io_method,
		$psql_a,
		"normal rel: test reading of invalid block 3",
		qq(
SELECT read_rel_block_ll('tbl_normal', 3, nblocks=>1, zero_on_error=>false);),
		qr/^$/,
		qr/^psql:<stdin>:\d+: ERROR:  invalid page in block 3 of relation base\/\d+\/\d+$/
	);

	my ($cs_count_after, $cs_ts_after) =
	  checksum_failures($psql_a, 'postgres');

	cmp_ok($cs_count_before + 1,
		'<=', $cs_count_after,
		"$io_method: normal rel: checksum count increased");
	cmp_ok($cs_ts_after, 'ne', '',
		"$io_method: normal rel: checksum timestamp is not null");


	# Check that page validity errors are detected, checksums stats increase, temp rel
	($cs_count_after, $cs_ts_after) = checksum_failures($psql_a, 'postgres');
	psql_like(
		$io_method,
		$psql_a,
		"temp rel: test reading of invalid block 4, valid block 5",
		qq(
SELECT read_rel_block_ll('tbl_temp', 4, nblocks=>2, zero_on_error=>false);),
		qr/^$/,
		qr/^psql:<stdin>:\d+: ERROR:  invalid page in block 4 of relation base\/\d+\/t\d+_\d+$/
	);

	($cs_count_after, $cs_ts_after) = checksum_failures($psql_a, 'postgres');

	cmp_ok($cs_count_before + 1,
		'<=', $cs_count_after,
		"$io_method: temp rel: checksum count increased");
	cmp_ok($cs_ts_after, 'ne', '',
		"$io_method: temp rel: checksum timestamp is not null");


	# Check that page validity errors are detected, checksums stats increase, shared rel
	($cs_count_before, $cs_ts_after) = checksum_failures($psql_a);
	psql_like(
		$io_method,
		$psql_a,
		"shared rel: reading of invalid blocks 2+3",
		qq(
SELECT read_rel_block_ll('pg_shseclabel', 2, nblocks=>2, zero_on_error=>false);),
		qr/^$/,
		qr/^psql:<stdin>:\d+: ERROR:  2 invalid pages among blocks 2..3 of relation global\/\d+\nDETAIL:  Block 2 held first invalid page\.\nHINT:[^\n]+$/
	);

	($cs_count_after, $cs_ts_after) = checksum_failures($psql_a);

	cmp_ok($cs_count_before + 1,
		'<=', $cs_count_after,
		"$io_method: shared rel: checksum count increased");
	cmp_ok($cs_ts_after, 'ne', '',
		"$io_method: shared rel: checksum timestamp is not null");


	# and restore sanity
	$psql_a->query(
		qq(
SELECT modify_rel_block('pg_shseclabel', 1, zero=>true);
DROP TABLE tbl_normal;
));
	$psql_a->{stderr} = '';

	$psql_a->quit();
}

# Verify checksum handling when creating database from a database with an
# invalid block. This also serves as a minimal check that cross-database IO is
# handled reasonably.
sub test_checksum_createdb
{
	my $io_method = shift;
	my $node = shift;

	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	$node->safe_psql('postgres',
		'CREATE DATABASE regression_createdb_source');

	$node->safe_psql(
		'regression_createdb_source', qq(
CREATE EXTENSION test_aio;
CREATE TABLE tbl_cs_fail(data int not null) WITH (AUTOVACUUM_ENABLED = false);
INSERT INTO tbl_cs_fail SELECT generate_series(1, 1000);
SELECT modify_rel_block('tbl_cs_fail', 1, corrupt_checksum=>true);
));

	my $createdb_sql = qq(
CREATE DATABASE regression_createdb_target
TEMPLATE regression_createdb_source
STRATEGY wal_log;
);

	# Verify that CREATE DATABASE of an invalid database fails and is
	# accounted for accurately.
	#
	# Note: On windows additional WARNING messages might be printed, due to
	# "some useless files may be left behind" warnings. While we probably
	# should prevent those from occurring, they're independent of AIO, so we
	# shouldn't fail because of them here.
	my ($cs_count_before, $cs_ts_before) =
	  checksum_failures($psql, 'regression_createdb_source');
	psql_like(
		$io_method,
		$psql,
		"create database w/ wal strategy, invalid source",
		$createdb_sql,
		qr/^$/,
		qr/psql:<stdin>:\d+: ERROR:  invalid page in block 1 of relation base\/\d+\/\d+$/
	);
	my ($cs_count_after, $cs_ts_after) =
	  checksum_failures($psql, 'regression_createdb_source');
	cmp_ok($cs_count_before + 1, '<=', $cs_count_after,
		"$io_method: create database w/ wal strategy, invalid source: checksum count increased"
	);

	# Verify that CREATE DATABASE of the fixed database succeeds.
	$node->safe_psql(
		'regression_createdb_source', qq(
SELECT modify_rel_block('tbl_cs_fail', 1, zero=>true);
));
	psql_like($io_method, $psql,
		"create database w/ wal strategy, valid source",
		$createdb_sql, qr/^$/, qr/^$/);

	$psql->quit();
}

# Test that we detect checksum failures and report them
#
# In several places we make sure that the server log actually contains
# individual information for each block involved in the IO.
sub test_ignore_checksum
{
	my $io_method = shift;
	my $node = shift;

	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	# Test setup
	$psql->query_safe(
		qq(
CREATE TABLE tbl_cs_fail(id int) WITH (AUTOVACUUM_ENABLED = false);
INSERT INTO tbl_cs_fail SELECT generate_series(1, 10000);
));

	my $count_sql = "SELECT count(*) FROM tbl_cs_fail";
	my $invalidate_sql = qq(
SELECT invalidate_rel_block('tbl_cs_fail', g.i)
FROM generate_series(0, 6) g(i);
);

	my $expect = $psql->query_safe($count_sql);


	# Very basic tests for ignore_checksum_failure=off / on

	$psql->query_safe(
		qq(
SELECT modify_rel_block('tbl_cs_fail', 1, corrupt_checksum=>true);
SELECT modify_rel_block('tbl_cs_fail', 5, corrupt_checksum=>true);
SELECT modify_rel_block('tbl_cs_fail', 6, corrupt_checksum=>true);
));

	$psql->query_safe($invalidate_sql);
	psql_like(
		$io_method,
		$psql,
		"reading block w/ wrong checksum with ignore_checksum_failure=off fails",
		$count_sql,
		qr/^$/,
		qr/ERROR:  invalid page in block/);

	$psql->query_safe("SET ignore_checksum_failure=on");

	$psql->query_safe($invalidate_sql);
	psql_like(
		$io_method,
		$psql,
		"reading block w/ wrong checksum with ignore_checksum_failure=off succeeds",
		$count_sql,
		qr/^$expect$/,
		qr/WARNING:  ignoring (checksum failure|\d checksum failures)/);


	# Verify that ignore_checksum_failure=off works in multi-block reads

	$psql->query_safe(
		qq(
SELECT modify_rel_block('tbl_cs_fail', 2, zero=>true);
SELECT modify_rel_block('tbl_cs_fail', 3, corrupt_checksum=>true);
SELECT modify_rel_block('tbl_cs_fail', 4, corrupt_header=>true);
));

	my $log_location = -s $node->logfile;
	psql_like(
		$io_method,
		$psql,
		"test reading of checksum failed block 3, with ignore",
		qq(
SELECT read_rel_block_ll('tbl_cs_fail', 3, nblocks=>1, zero_on_error=>false);),
		qr/^$/,
		qr/^psql:<stdin>:\d+: WARNING:  ignoring checksum failure in block 3/
	);

	# Check that the log contains a LOG message about the failure
	$log_location =
	  $node->wait_for_log(qr/LOG:  ignoring checksum failure/, $log_location);

	# check that we error
	psql_like(
		$io_method,
		$psql,
		"test reading of valid block 2, checksum failed 3, invalid 4, zero=false with ignore",
		qq(
SELECT read_rel_block_ll('tbl_cs_fail', 2, nblocks=>3, zero_on_error=>false);),
		qr/^$/,
		qr/^psql:<stdin>:\d+: ERROR:  invalid page in block 4 of relation base\/\d+\/\d+$/
	);

	# Test multi-block read with different problems in different blocks
	$psql->query(
		qq(
SELECT modify_rel_block('tbl_cs_fail', 1, zero=>true);
SELECT modify_rel_block('tbl_cs_fail', 2, corrupt_checksum=>true);
SELECT modify_rel_block('tbl_cs_fail', 3, corrupt_checksum=>true, corrupt_header=>true);
SELECT modify_rel_block('tbl_cs_fail', 4, corrupt_header=>true);
SELECT modify_rel_block('tbl_cs_fail', 5, corrupt_header=>true);
));
	$psql->{stderr} = '';

	$log_location = -s $node->logfile;
	psql_like(
		$io_method,
		$psql,
		"test reading of valid block 1, checksum failed 2, 3, invalid 3-5, zero=true",
		qq(
SELECT read_rel_block_ll('tbl_cs_fail', 1, nblocks=>5, zero_on_error=>true);),
		qr/^$/,
		qr/^psql:<stdin>:\d+: WARNING:  zeroing 3 page\(s\) and ignoring 2 checksum failure\(s\) among blocks 1..5 of relation/
	);


	# Unfortunately have to scan the whole log since determining $log_location
	# above in each of the tests, as wait_for_log() returns the size of the
	# file.

	$node->wait_for_log(qr/LOG:  ignoring checksum failure in block 2/,
		$log_location);
	ok(1, "$io_method: found information about checksum failure in block 2");

	$node->wait_for_log(
		qr/LOG:  invalid page in block 3 of relation base.*; zeroing out page/,
		$log_location);
	ok(1, "$io_method: found information about invalid page in block 3");

	$node->wait_for_log(
		qr/LOG:  invalid page in block 4 of relation base.*; zeroing out page/,
		$log_location);
	ok(1, "$io_method: found information about checksum failure in block 4");

	$node->wait_for_log(
		qr/LOG:  invalid page in block 5 of relation base.*; zeroing out page/,
		$log_location);
	ok(1, "$io_method: found information about checksum failure in block 5");


	# Reading a page with both an invalid header and an invalid checksum
	$psql->query(
		qq(
SELECT modify_rel_block('tbl_cs_fail', 3, corrupt_checksum=>true, corrupt_header=>true);
));
	$psql->{stderr} = '';

	psql_like(
		$io_method,
		$psql,
		"test reading of block with both invalid header and invalid checksum, zero=false",
		qq(
SELECT read_rel_block_ll('tbl_cs_fail', 3, nblocks=>1, zero_on_error=>false);),
		qr/^$/,
		qr/^psql:<stdin>:\d+: ERROR:  invalid page in block 3 of relation/);

	psql_like(
		$io_method,
		$psql,
		"test reading of block 3 with both invalid header and invalid checksum, zero=true",
		qq(
SELECT read_rel_block_ll('tbl_cs_fail', 3, nblocks=>1, zero_on_error=>true);),
		qr/^$/,
		qr/^psql:<stdin>:\d+: WARNING:  invalid page in block 3 of relation base\/.*; zeroing out page/
	);


	$psql->quit();
}


# Run all tests that are supported for all io_methods
sub test_generic
{
	my $io_method = shift;
	my $node = shift;

	is($node->safe_psql('postgres', 'SHOW io_method'),
		$io_method, "$io_method: io_method set correctly");

	$node->safe_psql(
		'postgres', qq(
CREATE EXTENSION test_aio;
CREATE TABLE tbl_corr(data int not null) WITH (AUTOVACUUM_ENABLED = false);
CREATE TABLE tbl_ok(data int not null) WITH (AUTOVACUUM_ENABLED = false);

INSERT INTO tbl_corr SELECT generate_series(1, 10000);
INSERT INTO tbl_ok SELECT generate_series(1, 10000);
SELECT grow_rel('tbl_corr', 16);
SELECT grow_rel('tbl_ok', 16);

SELECT modify_rel_block('tbl_corr', 1, corrupt_header=>true);
CHECKPOINT;
));

	test_handle($io_method, $node);
	test_io_error($io_method, $node);
	test_batchmode($io_method, $node);
	test_startwait_io($io_method, $node);
	test_complete_foreign($io_method, $node);
	test_close_fd($io_method, $node);
	test_invalidate($io_method, $node);
	test_zero($io_method, $node);
	test_checksum($io_method, $node);
	test_ignore_checksum($io_method, $node);
	test_checksum_createdb($io_method, $node);

  SKIP:
	{
		skip 'Injection points not supported by this build', 1
		  unless $ENV{enable_injection_points} eq 'yes';
		test_inject($io_method, $node);
	}
}
