# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Verify crash recovery behavior when the WAL segment containing the
# checkpoint record referenced by pg_controldata is missing.  This
# checks the code path where there is no backup_label file, where the
# startup process should fail with FATAL and log a message about the
# missing checkpoint record.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('testnode');
$node->init;
$node->append_conf('postgresql.conf', 'log_checkpoints = on');
$node->start;

# Force a checkpoint so as pg_controldata points to a checkpoint record we
# can target.
$node->safe_psql('postgres', 'CHECKPOINT;');

# Retrieve the checkpoint LSN and derive the WAL segment name.
my $checkpoint_walfile = $node->safe_psql('postgres',
	"SELECT pg_walfile_name(checkpoint_lsn) FROM pg_control_checkpoint()");

ok($checkpoint_walfile ne '',
	"derived checkpoint WAL file name: $checkpoint_walfile");

# Stop the node.
$node->stop('immediate');

# Remove the WAL segment containing the checkpoint record.
my $walpath = $node->data_dir . "/pg_wal/$checkpoint_walfile";
ok(-f $walpath, "checkpoint WAL file exists before deletion: $walpath");

unlink $walpath
  or die "could not remove WAL file $walpath: $!";

ok(!-e $walpath, "checkpoint WAL file removed: $walpath");

# Use run_log instead of node->start because this test expects that
# the server ends with an error during recovery.
run_log(
	[
		'pg_ctl',
		'--pgdata' => $node->data_dir,
		'--log' => $node->logfile,
		'start',
	]);

# Confirm that recovery has failed as expected.
my $logfile = slurp_file($node->logfile());
ok( $logfile =~
	  qr/FATAL: .* could not locate a valid checkpoint record at .*/,
	"FATAL logged for missing checkpoint record (no backup_label path)");

done_testing();
