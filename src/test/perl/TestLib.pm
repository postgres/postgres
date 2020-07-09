
=pod

=head1 NAME

TestLib - helper module for writing PostgreSQL's C<prove> tests.

=head1 SYNOPSIS

  use TestLib;

  # Test basic output of a command
  program_help_ok('initdb');
  program_version_ok('initdb');
  program_options_handling_ok('initdb');

  # Test option combinations
  command_fails(['initdb', '--invalid-option'],
              'command fails with invalid option');
  my $tempdir = TestLib::tempdir;
  command_ok('initdb', '-D', $tempdir);

  # Miscellanea
  print "on Windows" if $TestLib::windows_os;
  my $path = TestLib::perl2host($backup_dir);
  ok(check_mode_recursive($stream_dir, 0700, 0600),
    "check stream dir permissions");
  TestLib::system_log('pg_ctl', 'kill', 'QUIT', $slow_pid);

=head1 DESCRIPTION

C<TestLib> contains a set of routines dedicated to environment setup for
a PostgreSQL regression test run and includes some low-level routines
aimed at controlling command execution, logging and test functions.

=cut

# This module should never depend on any other PostgreSQL regression test
# modules.

package TestLib;

use strict;
use warnings;

use Config;
use Cwd;
use Exporter 'import';
use Fcntl qw(:mode);
use File::Basename;
use File::Find;
use File::Spec;
use File::stat qw(stat);
use File::Temp ();
use IPC::Run;
use SimpleTee;

# specify a recent enough version of Test::More to support the
# done_testing() function
use Test::More 0.87;

our @EXPORT = qw(
  generate_ascii_string
  slurp_dir
  slurp_file
  append_to_file
  check_mode_recursive
  chmod_recursive
  check_pg_config
  system_or_bail
  system_log
  run_log
  run_command

  command_ok
  command_fails
  command_exit_is
  program_help_ok
  program_version_ok
  program_options_handling_ok
  command_like
  command_like_safe
  command_fails_like
  command_checks_all

  $windows_os
  $use_unix_sockets
);

our ($windows_os, $use_unix_sockets, $tmp_check, $log_path, $test_logfile);

BEGIN
{

	# Set to untranslated messages, to be able to compare program output
	# with expected strings.
	delete $ENV{LANGUAGE};
	delete $ENV{LC_ALL};
	$ENV{LC_MESSAGES} = 'C';

	delete $ENV{PGCONNECT_TIMEOUT};
	delete $ENV{PGDATA};
	delete $ENV{PGDATABASE};
	delete $ENV{PGHOSTADDR};
	delete $ENV{PGREQUIRESSL};
	delete $ENV{PGSERVICE};
	delete $ENV{PGSSLMODE};
	delete $ENV{PGUSER};
	delete $ENV{PGPORT};
	delete $ENV{PGHOST};
	delete $ENV{PG_COLOR};

	$ENV{PGAPPNAME} = basename($0);

	# Must be set early
	$windows_os = $Config{osname} eq 'MSWin32' || $Config{osname} eq 'msys';
	if ($windows_os)
	{
		require Win32API::File;
		Win32API::File->import(qw(createFile OsFHandleOpen CloseHandle));
	}

	# Specifies whether to use Unix sockets for test setups.  On
	# Windows we don't use them by default since it's not universally
	# supported, but it can be overridden if desired.
	$use_unix_sockets =
	  (!$windows_os || defined $ENV{PG_TEST_USE_UNIX_SOCKETS});
}

=pod

=head1 EXPORTED VARIABLES

=over

=item C<$windows_os>

Set to true when running under Windows, except on Cygwin.

=back

=cut

INIT
{

	# Return EPIPE instead of killing the process with SIGPIPE.  An affected
	# test may still fail, but it's more likely to report useful facts.
	$SIG{PIPE} = 'IGNORE';

	# Determine output directories, and create them.  The base path is the
	# TESTDIR environment variable, which is normally set by the invoking
	# Makefile.
	$tmp_check = $ENV{TESTDIR} ? "$ENV{TESTDIR}/tmp_check" : "tmp_check";
	$log_path = "$tmp_check/log";

	mkdir $tmp_check;
	mkdir $log_path;

	# Open the test log file, whose name depends on the test name.
	$test_logfile = basename($0);
	$test_logfile =~ s/\.[^.]+$//;
	$test_logfile = "$log_path/regress_log_$test_logfile";
	open my $testlog, '>', $test_logfile
	  or die "could not open STDOUT to logfile \"$test_logfile\": $!";

	# Hijack STDOUT and STDERR to the log file
	open(my $orig_stdout, '>&', \*STDOUT);
	open(my $orig_stderr, '>&', \*STDERR);
	open(STDOUT,          '>&', $testlog);
	open(STDERR,          '>&', $testlog);

	# The test output (ok ...) needs to be printed to the original STDOUT so
	# that the 'prove' program can parse it, and display it to the user in
	# real time. But also copy it to the log file, to provide more context
	# in the log.
	my $builder = Test::More->builder;
	my $fh      = $builder->output;
	tie *$fh, "SimpleTee", $orig_stdout, $testlog;
	$fh = $builder->failure_output;
	tie *$fh, "SimpleTee", $orig_stderr, $testlog;

	# Enable auto-flushing for all the file handles. Stderr and stdout are
	# redirected to the same file, and buffering causes the lines to appear
	# in the log in confusing order.
	autoflush STDOUT 1;
	autoflush STDERR 1;
	autoflush $testlog 1;
}

END
{

	# Test files have several ways of causing prove_check to fail:
	# 1. Exit with a non-zero status.
	# 2. Call ok(0) or similar, indicating that a constituent test failed.
	# 3. Deviate from the planned number of tests.
	#
	# Preserve temporary directories after (1) and after (2).
	$File::Temp::KEEP_ALL = 1 unless $? == 0 && all_tests_passing();
}

=pod

=head1 ROUTINES

=over

=item all_tests_passing()

Return 1 if all the tests run so far have passed. Otherwise, return 0.

=cut

sub all_tests_passing
{
	foreach my $status (Test::More->builder->summary)
	{
		return 0 unless $status;
	}
	return 1;
}

=pod

=item tempdir(prefix)

Securely create a temporary directory inside C<$tmp_check>, like C<mkdtemp>,
and return its name.  The directory will be removed automatically at the
end of the tests.

If C<prefix> is given, the new directory is templated as C<${prefix}_XXXX>.
Otherwise the template is C<tmp_test_XXXX>.

=cut

sub tempdir
{
	my ($prefix) = @_;
	$prefix = "tmp_test" unless defined $prefix;
	return File::Temp::tempdir(
		$prefix . '_XXXX',
		DIR     => $tmp_check,
		CLEANUP => 1);
}

=pod

=item tempdir_short()

As above, but the directory is outside the build tree so that it has a short
name, to avoid path length issues.

=cut

sub tempdir_short
{

	return File::Temp::tempdir(CLEANUP => 1);
}

=pod

=item perl2host()

Translate a Perl file name to a host file name.  Currently, this is a no-op
except for the case of Perl=msys and host=mingw32.  The subject need not
exist, but its parent directory must exist.

=cut

sub perl2host
{
	my ($subject) = @_;
	return $subject unless $Config{osname} eq 'msys';
	my $here = cwd;
	my $leaf;
	if (chdir $subject)
	{
		$leaf = '';
	}
	else
	{
		$leaf = '/' . basename $subject;
		my $parent = dirname $subject;
		chdir $parent or die "could not chdir \"$parent\": $!";
	}

	# this odd way of calling 'pwd -W' is the only way that seems to work.
	my $dir = qx{sh -c "pwd -W"};
	chomp $dir;
	chdir $here;
	return $dir . $leaf;
}

=pod

=item system_log(@cmd)

Run (via C<system()>) the command passed as argument; the return
value is passed through.

=cut

sub system_log
{
	print("# Running: " . join(" ", @_) . "\n");
	return system(@_);
}

=pod

=item system_or_bail(@cmd)

Run (via C<system()>) the command passed as argument, and returns
if the command is successful.
On failure, abandon further tests and exit the program.

=cut

sub system_or_bail
{
	if (system_log(@_) != 0)
	{
		BAIL_OUT("system $_[0] failed");
	}
	return;
}

=pod

=item run_log(@cmd)

Run the given command via C<IPC::Run::run()>, noting it in the log.
The return value from the command is passed through.

=cut

sub run_log
{
	print("# Running: " . join(" ", @{ $_[0] }) . "\n");
	return IPC::Run::run(@_);
}

=pod

=item run_command(cmd)

Run (via C<IPC::Run::run()>) the command passed as argument.
The return value from the command is ignored.
The return value is C<($stdout, $stderr)>.

=cut

sub run_command
{
	my ($cmd) = @_;
	my ($stdout, $stderr);
	my $result = IPC::Run::run $cmd, '>', \$stdout, '2>', \$stderr;
	chomp($stdout);
	chomp($stderr);
	return ($stdout, $stderr);
}

=pod

=item generate_ascii_string(from_char, to_char)

Generate a string made of the given range of ASCII characters.

=cut

sub generate_ascii_string
{
	my ($from_char, $to_char) = @_;
	my $res;

	for my $i ($from_char .. $to_char)
	{
		$res .= sprintf("%c", $i);
	}
	return $res;
}

=pod

=item slurp_dir(dir)

Return the complete list of entries in the specified directory.

=cut

sub slurp_dir
{
	my ($dir) = @_;
	opendir(my $dh, $dir)
	  or die "could not opendir \"$dir\": $!";
	my @direntries = readdir $dh;
	closedir $dh;
	return @direntries;
}

=pod

=item slurp_file(filename)

Return the full contents of the specified file.

=cut

sub slurp_file
{
	my ($filename) = @_;
	local $/;
	my $contents;
	if ($Config{osname} ne 'MSWin32')
	{
		open(my $in, '<', $filename)
		  or die "could not read \"$filename\": $!";
		$contents = <$in>;
		close $in;
	}
	else
	{
		my $fHandle = createFile($filename, "r", "rwd")
		  or die "could not open \"$filename\": $^E";
		OsFHandleOpen(my $fh = IO::Handle->new(), $fHandle, 'r')
		  or die "could not read \"$filename\": $^E\n";
		$contents = <$fh>;
		CloseHandle($fHandle)
		  or die "could not close \"$filename\": $^E\n";
	}
	$contents =~ s/\r\n/\n/g if $Config{osname} eq 'msys';
	return $contents;
}

=pod

=item append_to_file(filename, str)

Append a string at the end of a given file.  (Note: no newline is appended at
end of file.)

=cut

sub append_to_file
{
	my ($filename, $str) = @_;
	open my $fh, ">>", $filename
	  or die "could not write \"$filename\": $!";
	print $fh $str;
	close $fh;
	return;
}

=pod

=item check_mode_recursive(dir, expected_dir_mode, expected_file_mode, ignore_list)

Check that all file/dir modes in a directory match the expected values,
ignoring files in C<ignore_list> (basename only).

=cut

sub check_mode_recursive
{
	my ($dir, $expected_dir_mode, $expected_file_mode, $ignore_list) = @_;

	# Result defaults to true
	my $result = 1;

	find(
		{
			follow_fast => 1,
			wanted      => sub {
				# Is file in the ignore list?
				foreach my $ignore ($ignore_list ? @{$ignore_list} : [])
				{
					if ("$dir/$ignore" eq $File::Find::name)
					{
						return;
					}
				}

				# Allow ENOENT.  A running server can delete files, such as
				# those in pg_stat.  Other stat() failures are fatal.
				my $file_stat = stat($File::Find::name);
				unless (defined($file_stat))
				{
					my $is_ENOENT = $!{ENOENT};
					my $msg       = "unable to stat $File::Find::name: $!";
					if ($is_ENOENT)
					{
						warn $msg;
						return;
					}
					else
					{
						die $msg;
					}
				}

				my $file_mode = S_IMODE($file_stat->mode);

				# Is this a file?
				if (S_ISREG($file_stat->mode))
				{
					if ($file_mode != $expected_file_mode)
					{
						print(
							*STDERR,
							sprintf("$File::Find::name mode must be %04o\n",
								$expected_file_mode));

						$result = 0;
						return;
					}
				}

				# Else a directory?
				elsif (S_ISDIR($file_stat->mode))
				{
					if ($file_mode != $expected_dir_mode)
					{
						print(
							*STDERR,
							sprintf("$File::Find::name mode must be %04o\n",
								$expected_dir_mode));

						$result = 0;
						return;
					}
				}

				# Else something we can't handle
				else
				{
					die "unknown file type for $File::Find::name";
				}
			}
		},
		$dir);

	return $result;
}

=pod

=item chmod_recursive(dir, dir_mode, file_mode)

C<chmod> recursively each file and directory within the given directory.

=cut

sub chmod_recursive
{
	my ($dir, $dir_mode, $file_mode) = @_;

	find(
		{
			follow_fast => 1,
			wanted      => sub {
				my $file_stat = stat($File::Find::name);

				if (defined($file_stat))
				{
					chmod(
						S_ISDIR($file_stat->mode) ? $dir_mode : $file_mode,
						$File::Find::name
					) or die "unable to chmod $File::Find::name";
				}
			}
		},
		$dir);
	return;
}

=pod

=item check_pg_config(regexp)

Return the number of matches of the given regular expression
within the installation's C<pg_config.h>.

=cut

sub check_pg_config
{
	my ($regexp) = @_;
	my ($stdout, $stderr);
	my $result = IPC::Run::run [ 'pg_config', '--includedir' ], '>',
	  \$stdout, '2>', \$stderr
	  or die "could not execute pg_config";
	chomp($stdout);
	$stdout =~ s/\r$//;

	open my $pg_config_h, '<', "$stdout/pg_config.h" or die "$!";
	my $match = (grep { /^$regexp/ } <$pg_config_h>);
	close $pg_config_h;
	return $match;
}

=pod

=back

=head1 Test::More-LIKE METHODS

=over

=item command_ok(cmd, test_name)

Check that the command runs (via C<run_log>) successfully.

=cut

sub command_ok
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd, $test_name) = @_;
	my $result = run_log($cmd);
	ok($result, $test_name);
	return;
}

=pod

=item command_fails(cmd, test_name)

Check that the command fails (when run via C<run_log>).

=cut

sub command_fails
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd, $test_name) = @_;
	my $result = run_log($cmd);
	ok(!$result, $test_name);
	return;
}

=pod

=item command_exit_is(cmd, expected, test_name)

Check that the command exit code matches the expected exit code.

=cut

sub command_exit_is
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd, $expected, $test_name) = @_;
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	my $h = IPC::Run::start $cmd;
	$h->finish();

	# On Windows, the exit status of the process is returned directly as the
	# process's exit code, while on Unix, it's returned in the high bits
	# of the exit code (see WEXITSTATUS macro in the standard <sys/wait.h>
	# header file). IPC::Run's result function always returns exit code >> 8,
	# assuming the Unix convention, which will always return 0 on Windows as
	# long as the process was not terminated by an exception. To work around
	# that, use $h->full_results on Windows instead.
	my $result =
	    ($Config{osname} eq "MSWin32")
	  ? ($h->full_results)[0]
	  : $h->result(0);
	is($result, $expected, $test_name);
	return;
}

=pod

=item program_help_ok(cmd)

Check that the command supports the C<--help> option.

=cut

sub program_help_ok
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd) = @_;
	my ($stdout, $stderr);
	print("# Running: $cmd --help\n");
	my $result = IPC::Run::run [ $cmd, '--help' ], '>', \$stdout, '2>',
	  \$stderr;
	ok($result, "$cmd --help exit code 0");
	isnt($stdout, '', "$cmd --help goes to stdout");
	is($stderr, '', "$cmd --help nothing to stderr");
	return;
}

=pod

=item program_version_ok(cmd)

Check that the command supports the C<--version> option.

=cut

sub program_version_ok
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd) = @_;
	my ($stdout, $stderr);
	print("# Running: $cmd --version\n");
	my $result = IPC::Run::run [ $cmd, '--version' ], '>', \$stdout, '2>',
	  \$stderr;
	ok($result, "$cmd --version exit code 0");
	isnt($stdout, '', "$cmd --version goes to stdout");
	is($stderr, '', "$cmd --version nothing to stderr");
	return;
}

=pod

=item program_options_handling_ok(cmd)

Check that a command with an invalid option returns a non-zero
exit code and error message.

=cut

sub program_options_handling_ok
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd) = @_;
	my ($stdout, $stderr);
	print("# Running: $cmd --not-a-valid-option\n");
	my $result = IPC::Run::run [ $cmd, '--not-a-valid-option' ], '>',
	  \$stdout,
	  '2>', \$stderr;
	ok(!$result, "$cmd with invalid option nonzero exit code");
	isnt($stderr, '', "$cmd with invalid option prints error message");
	return;
}

=pod

=item command_like(cmd, expected_stdout, test_name)

Check that the command runs successfully and the output
matches the given regular expression.

=cut

sub command_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd, $expected_stdout, $test_name) = @_;
	my ($stdout, $stderr);
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	my $result = IPC::Run::run $cmd, '>', \$stdout, '2>', \$stderr;
	ok($result, "$test_name: exit code 0");
	is($stderr, '', "$test_name: no stderr");
	like($stdout, $expected_stdout, "$test_name: matches");
	return;
}

=pod

=item command_like_safe(cmd, expected_stdout, test_name)

Check that the command runs successfully and the output
matches the given regular expression.  Doesn't assume that the
output files are closed.

=cut

sub command_like_safe
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	# Doesn't rely on detecting end of file on the file descriptors,
	# which can fail, causing the process to hang, notably on Msys
	# when used with 'pg_ctl start'
	my ($cmd, $expected_stdout, $test_name) = @_;
	my ($stdout, $stderr);
	my $stdoutfile = File::Temp->new();
	my $stderrfile = File::Temp->new();
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	my $result = IPC::Run::run $cmd, '>', $stdoutfile, '2>', $stderrfile;
	$stdout = slurp_file($stdoutfile);
	$stderr = slurp_file($stderrfile);
	ok($result, "$test_name: exit code 0");
	is($stderr, '', "$test_name: no stderr");
	like($stdout, $expected_stdout, "$test_name: matches");
	return;
}

=pod

=item command_fails_like(cmd, expected_stderr, test_name)

Check that the command fails and the error message matches
the given regular expression.

=cut

sub command_fails_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd, $expected_stderr, $test_name) = @_;
	my ($stdout, $stderr);
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	my $result = IPC::Run::run $cmd, '>', \$stdout, '2>', \$stderr;
	ok(!$result, "$test_name: exit code not 0");
	like($stderr, $expected_stderr, "$test_name: matches");
	return;
}

=pod

=item command_checks_all(cmd, ret, out, err, test_name)

Run a command and check its status and outputs.
Arguments:

=over

=item C<cmd>: Array reference of command and arguments to run

=item C<ret>: Expected exit code

=item C<out>: Expected stdout from command

=item C<err>: Expected stderr from command

=item C<test_name>: test name

=back

=cut

sub command_checks_all
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($cmd, $expected_ret, $out, $err, $test_name) = @_;

	# run command
	my ($stdout, $stderr);
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	IPC::Run::run($cmd, '>', \$stdout, '2>', \$stderr);

	# See http://perldoc.perl.org/perlvar.html#%24CHILD_ERROR
	my $ret = $?;
	die "command exited with signal " . ($ret & 127)
	  if $ret & 127;
	$ret = $ret >> 8;

	# check status
	ok($ret == $expected_ret,
		"$test_name status (got $ret vs expected $expected_ret)");

	# check stdout
	for my $re (@$out)
	{
		like($stdout, $re, "$test_name stdout /$re/");
	}

	# check stderr
	for my $re (@$err)
	{
		like($stderr, $re, "$test_name stderr /$re/");
	}

	return;
}

=pod

=back

=cut

1;
