use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 1;
use Time::HiRes qw(usleep);

my $tempdir = TestLib::tempdir;

my $node = get_new_node('primary');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', qq(
logging_collector = on
log_directory = 'log'
log_filename = 'postgresql.log'
));

$node->start();

# Rename log file and rotate log.  Then log file should appear again.

my $logfile = $node->data_dir . '/log/postgresql.log';
my $old_logfile = $node->data_dir . '/log/postgresql.old';
rename($logfile, $old_logfile);

$node->logrotate();

# pg_ctl logrotate doesn't wait until rotation request being completed.  So
# we have to wait some time until log file appears.
my $attempts = 0;
my $max_attempts = 180 * 10;
while (not -e $logfile and $attempts < $max_attempts)
{
	usleep(100_000);
	$attempts++;
}

ok(-e $logfile, "log file exists");

$node->stop();
