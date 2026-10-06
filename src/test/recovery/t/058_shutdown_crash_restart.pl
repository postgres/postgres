# Copyright (c) 2026, PostgreSQL Global Development Group

# Test shutdown during a crash restart, before WAL redo has started.  The
# test relies on a fake restore_command that keeps the startup process at
# some early stage, waiting for a shutdown to happen, with a checkpointer
# spawned and running.

use strict;
use warnings FATAL => 'all';
use FindBin;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init(allows_streaming => 1);

# Make the restarted startup process wait in restore_command until shutdown.
my $perlbin = $^X;
$perlbin =~ s!\\!/!g if $windows_os;
my $logfile = $node->logfile;
$logfile =~ s!\\!/!g if $windows_os;
my $restore_timeout = $PostgreSQL::Test::Utils::timeout_default;

# DEBUG2 is required for the checkpointer log entry lookup.
$node->append_conf(
	'postgresql.conf', qq{
restart_after_crash = on
log_min_messages = debug2
restore_command = '"$perlbin" "$FindBin::RealBin/wait_for_shutdown" "$logfile" $restore_timeout'
});
$node->start;

# Stop the background writer.
$node->poll_query_until(
	'postgres',
	q{SELECT count(*) = 1 FROM pg_stat_activity
	  WHERE backend_type = 'background writer'}
) or die 'background writer did not start';
my $pid = $node->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE backend_type = 'background writer'"
);
$node->set_standby_mode;
my $log_offset = -s $node->logfile;
is(PostgreSQL::Test::Utils::system_log('pg_ctl', 'kill', 'QUIT', $pid),
	0, "SIGQUIT sent to background writer");
$node->wait_for_log(qr/restore_command waiting for shutdown/, $log_offset);

# Wait until the new checkpointer has installed its SIGTERM handler.
$node->wait_for_log(
	qr/checkpointer updated shared memory configuration values/, $log_offset);

ok($node->stop('fast', fail_ok => 1),
	'fast shutdown completes during crash restart');

unlike(
	slurp_file($node->logfile, $log_offset),
	qr/timed out waiting for shutdown request/,
	'restore_command did not time out');

done_testing();
