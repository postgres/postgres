use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 5;
use Time::HiRes qw(usleep);

# Set up node with logging collector
my $node = get_new_node('primary');
$node->init();
$node->append_conf(
	'postgresql.conf', qq(
logging_collector = on
# these ensure stability of test results:
log_rotation_age = 0
lc_messages = 'C'
));

$node->start();

# Verify that log output gets to the file

$node->psql('postgres', 'SELECT 1/0');

# might need to retry if logging collector process is slow...
my $max_attempts = 10 * $TestLib::timeout_default;

my $current_logfiles;
for (my $attempts = 0; $attempts < $max_attempts; $attempts++)
{
	eval {
		$current_logfiles = slurp_file($node->data_dir . '/current_logfiles');
	};
	last unless $@;
	usleep(100_000);
}
die $@ if $@;

note "current_logfiles = $current_logfiles";

like(
	$current_logfiles,
	qr|^stderr log/postgresql-.*log$|,
	'current_logfiles is sane');

my $lfname = $current_logfiles;
$lfname =~ s/^stderr //;
chomp $lfname;

my $first_logfile;
for (my $attempts = 0; $attempts < $max_attempts; $attempts++)
{
	$first_logfile = slurp_file($node->data_dir . '/' . $lfname);
	last if $first_logfile =~ m/division by zero/;
	usleep(100_000);
}

like($first_logfile, qr/division by zero/, 'found expected log file content');

# While we're at it, test pg_current_logfile() function
is($node->safe_psql('postgres', "SELECT pg_current_logfile('stderr')"),
	$lfname, 'pg_current_logfile() gives correct answer');

# Sleep 2 seconds and ask for log rotation; this should result in
# output into a different log file name.
sleep(2);
$node->logrotate();

# pg_ctl logrotate doesn't wait for rotation request to be completed.
# Allow a bit of time for it to happen.
my $new_current_logfiles;
for (my $attempts = 0; $attempts < $max_attempts; $attempts++)
{
	$new_current_logfiles = slurp_file($node->data_dir . '/current_logfiles');
	last if $new_current_logfiles ne $current_logfiles;
	usleep(100_000);
}

note "now current_logfiles = $new_current_logfiles";

like(
	$new_current_logfiles,
	qr|^stderr log/postgresql-.*log$|,
	'new current_logfiles is sane');

$lfname = $new_current_logfiles;
$lfname =~ s/^stderr //;
chomp $lfname;

# Verify that log output gets to this file, too

$node->psql('postgres', 'fee fi fo fum');

my $second_logfile;
for (my $attempts = 0; $attempts < $max_attempts; $attempts++)
{
	$second_logfile = slurp_file($node->data_dir . '/' . $lfname);
	last if $second_logfile =~ m/syntax error/;
	usleep(100_000);
}

like(
	$second_logfile,
	qr/syntax error/,
	'found expected log file content in new log file');

$node->stop();
