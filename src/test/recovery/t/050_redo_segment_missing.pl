# Copyright (c) 2025-2026, PostgreSQL Global Development Group
#
# Evaluates PostgreSQL's recovery behavior when a WAL segment containing the
# redo record is missing, with a checkpoint record located in a different
# segment.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('testnode');
$node->init;
$node->append_conf('postgresql.conf', 'log_checkpoints = on');
$node->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', q(CREATE EXTENSION injection_points));

# Note that this uses two injection points based on waits, not one.  This
# may look strange, but this works as a workaround to enforce all memory
# allocations to happen outside the critical section of the checkpoint
# required for this test.
# First, "create-checkpoint-initial" is run outside the critical section
# section, and is used as a way to initialize the shared memory required
# for the wait machinery with its DSM registry.
# Then, "create-checkpoint-run" is loaded outside the critical section of
# a checkpoint to allocate any memory required by the library load, and
# its callback is run inside the critical section.
$node->safe_psql('postgres',
	q{select injection_points_attach('create-checkpoint-initial', 'wait')});
$node->safe_psql('postgres',
	q{select injection_points_attach('create-checkpoint-run', 'wait')});

# Start a psql session to run the checkpoint in the background and make
# the test wait on the injection point so the checkpoint stops just after
# it starts.
my $checkpoint = $node->background_psql('postgres');
$checkpoint->query_until(
	qr/starting_checkpoint/,
	q(\echo starting_checkpoint
checkpoint;
));

# Wait for the initial point to finish, the checkpointer is still
# outside its critical section.  Then release to reach the second
# point.
$node->wait_for_event('checkpointer', 'create-checkpoint-initial');
$node->safe_psql('postgres',
	q{select injection_points_wakeup('create-checkpoint-initial')});

# Wait until the checkpoint has reached the second injection point.
# We are now in the middle of a checkpoint running, after the redo
# record has been logged.
$node->wait_for_event('checkpointer', 'create-checkpoint-run');

# Switch the WAL segment, ensuring that the redo record will be included
# in a different segment than the checkpoint record.
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');

# Continue the checkpoint and wait for its completion.
my $log_offset = -s $node->logfile;
$node->safe_psql('postgres',
	q{select injection_points_wakeup('create-checkpoint-run')});
$node->wait_for_log(qr/checkpoint complete/, $log_offset);

$checkpoint->quit;

# Retrieve the WAL file names for the redo record and checkpoint record.
my $redo_lsn = $node->safe_psql('postgres',
	"SELECT redo_lsn FROM pg_control_checkpoint()");
my $redo_walfile_name =
  $node->safe_psql('postgres', "SELECT pg_walfile_name('$redo_lsn')");
my $checkpoint_lsn = $node->safe_psql('postgres',
	"SELECT checkpoint_lsn FROM pg_control_checkpoint()");
my $checkpoint_walfile_name =
  $node->safe_psql('postgres', "SELECT pg_walfile_name('$checkpoint_lsn')");

# Redo record and checkpoint record should be on different segments.
isnt($redo_walfile_name, $checkpoint_walfile_name,
	'redo and checkpoint records on different segments');

# Remove the WAL segment containing the redo record.
unlink $node->data_dir . "/pg_wal/$redo_walfile_name"
  or die "could not remove WAL file: $!";

$node->stop('immediate');

# Use run_log instead of node->start because this test expects that
# the server ends with an error during recovery.
run_log(
	[
		'pg_ctl',
		'--pgdata' => $node->data_dir,
		'--log' => $node->logfile,
		'start',
	]);

# Confirm that recovery has failed, as expected.
my $logfile = slurp_file($node->logfile());
ok( $logfile =~
	  qr/FATAL: .* could not find redo location .* referenced by checkpoint record at .*/,
	"ends with FATAL because it could not find redo location");

done_testing();
