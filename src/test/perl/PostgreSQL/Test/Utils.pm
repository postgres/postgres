
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

=pod

=head1 NAME

PostgreSQL::Test::Utils - helper module for writing PostgreSQL's C<prove> tests.

=head1 SYNOPSIS

  use PostgreSQL::Test::Utils;

  # Test basic output of a command
  program_help_ok('initdb');
  program_version_ok('initdb');
  program_options_handling_ok('initdb');

  # Test option combinations
  command_fails(['initdb', '--invalid-option'],
              'command fails with invalid option');
  my $tempdir = PostgreSQL::Test::Utils::tempdir;
  command_ok('initdb', '-D', $tempdir);

  # Miscellanea
  print "on Windows" if $PostgreSQL::Test::Utils::windows_os;
  ok(check_mode_recursive($stream_dir, 0700, 0600),
    "check stream dir permissions");
  PostgreSQL::Test::Utils::system_log('pg_ctl', 'kill', 'QUIT', $slow_pid);

=head1 DESCRIPTION

C<PostgreSQL::Test::Utils> contains a set of routines dedicated to environment setup for
a PostgreSQL regression test run and includes some low-level routines
aimed at controlling command execution, logging and test functions.

=cut

# This module should never depend on any other PostgreSQL regression test
# modules.

package PostgreSQL::Test::Utils;

use strict;
use warnings FATAL => 'all';

use Carp;
use Config;
use Cwd;
use Exporter 'import';
use Fcntl qw(:mode :seek);
use File::Basename;
use File::Compare;
use File::Find;
use File::Spec;
use File::stat qw(stat);
use File::Temp ();
use IPC::Run;
use POSIX qw(locale_h);
use PostgreSQL::Test::SimpleTee;

# We need a version of Test::More recent enough to support subtests
use Test::More 0.98;

our @EXPORT = qw(
  generate_ascii_string
  slurp_dir
  slurp_file
  append_to_file
  string_replace_file
  check_mode_recursive
  chmod_recursive
  check_pg_config
  compare_files
  dir_symlink
  scan_server_header
  system_or_bail
  system_log
  run_log
  run_command
  pump_until

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
  $is_msys2
  $use_unix_sockets
);

our ($windows_os, $is_msys2, $use_unix_sockets, $timeout_default,
	$tmp_check, $log_path, $test_logfile);

BEGIN
{

	# Set to untranslated messages, to be able to compare program output
	# with expected strings.
	delete $ENV{LANGUAGE};
	delete $ENV{LC_ALL};
	$ENV{LC_MESSAGES} = 'C';
	setlocale(LC_ALL, "");

	# This list should be kept in sync with pg_regress.c.
	my @envkeys = qw (
	  PGCHANNELBINDING
	  PGCLIENTENCODING
	  PGCONNECT_TIMEOUT
	  PGDATA
	  PGDATABASE
	  PGGSSDELEGATION
	  PGGSSENCMODE
	  PGGSSLIB
	  PGHOSTADDR
	  PGKRBSRVNAME
	  PGPASSFILE
	  PGPASSWORD
	  PGREQUIREPEER
	  PGREQUIRESSL
	  PGSERVICE
	  PGSERVICEFILE
	  PGSSLCERT
	  PGSSLCRL
	  PGSSLCRLDIR
	  PGSSLKEY
	  PGSSLMAXPROTOCOLVERSION
	  PGSSLMINPROTOCOLVERSION
	  PGSSLMODE
	  PGSSLROOTCERT
	  PGSSLSNI
	  PGTARGETSESSIONATTRS
	  PGUSER
	  PGPORT
	  PGHOST
	  PG_COLOR
	);
	delete @ENV{@envkeys};

	$ENV{PGAPPNAME} = basename($0);

	# Must be set early
	$windows_os = $Config{osname} eq 'MSWin32' || $Config{osname} eq 'msys';
	# Check if this environment is MSYS2.
	$is_msys2 =
		 $windows_os
	  && -x '/usr/bin/uname'
	  && `uname -or` =~ /^[2-9].*Msys/;

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

	$timeout_default = $ENV{PG_TEST_TIMEOUT_DEFAULT};
	$timeout_default = 180
	  if not defined $timeout_default or $timeout_default eq '';
}

=pod

=head1 EXPORTED VARIABLES

=over

=item C<$windows_os>

Set to true when running under Windows, except on Cygwin.

=item C<$is_msys2>

Set to true when running under MSYS2.

=back

=cut

INIT
{
	# See https://github.com/cpan-authors/IPC-Run/commit/fc9288c for how this
	# reduces idle time.  Remove this when IPC::Run 20231003.0 is too old to
	# matter (when all versions that matter provide the optimization).
	$SIG{CHLD} = sub { }
	  unless defined $SIG{CHLD};

	# Return EPIPE instead of killing the process with SIGPIPE.  An affected
	# test may still fail, but it's more likely to report useful facts.
	$SIG{PIPE} = 'IGNORE';

	# Determine output directories, and create them.  The base paths are the
	# TESTDATADIR / TESTLOGDIR environment variables, which are normally set
	# by the invoking Makefile.
	$tmp_check = $ENV{TESTDATADIR} ? "$ENV{TESTDATADIR}" : "tmp_check";
	$log_path = $ENV{TESTLOGDIR} ? "$ENV{TESTLOGDIR}" : "log";

	mkdir $tmp_check;
	mkdir $log_path;

	# Open the test log file, whose name depends on the test name.
	$test_logfile = basename($0);
	$test_logfile =~ s/\.[^.]+$//;
	$test_logfile = "$log_path/regress_log_$test_logfile";
	open my $testlog, '>', $test_logfile
	  or die "could not open STDOUT to logfile \"$test_logfile\": $!";

	# Hijack STDOUT and STDERR to the log file
	open(my $orig_stdout, '>&', \*STDOUT) or die $!;
	open(my $orig_stderr, '>&', \*STDERR) or die $!;
	open(STDOUT, '>&', $testlog) or die $!;
	open(STDERR, '>&', $testlog) or die $!;

	# The test output (ok ...) needs to be printed to the original STDOUT so
	# that the 'prove' program can parse it, and display it to the user in
	# real time. But also copy it to the log file, to provide more context
	# in the log.
	my $builder = Test::More->builder;
	my $fh = $builder->output;
	tie *$fh, "PostgreSQL::Test::SimpleTee", $orig_stdout, $testlog;
	$fh = $builder->failure_output;
	tie *$fh, "PostgreSQL::Test::SimpleTee", $orig_stderr, $testlog;

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
end of the tests, unless the environment variable PG_TEST_NOCLEAN is provided.

If C<prefix> is given, the new directory is templated as C<${prefix}_XXXX>.
Otherwise the template is C<tmp_test_XXXX>.

=cut

sub tempdir
{
	my ($prefix) = @_;
	$prefix = "tmp_test" unless defined $prefix;
	return File::Temp::tempdir(
		$prefix . '_XXXX',
		DIR => $tmp_check,
		CLEANUP => not defined $ENV{'PG_TEST_NOCLEAN'});
}

=pod

=item tempdir_short()

As above, but the directory is outside the build tree so that it has a short
name, to avoid path length issues.

=cut

sub tempdir_short
{

	return File::Temp::tempdir(
		CLEANUP => not defined $ENV{'PG_TEST_NOCLEAN'});
}

=pod

=item has_wal_read_bug()

Returns true if $tmp_check is subject to a sparc64+ext4 bug that causes WAL
readers to see zeros if another process simultaneously wrote the same offsets.
Consult this in tests that fail frequently on affected configurations.  The
bug has made streaming standbys fail to advance, reporting corrupt WAL.  It
has made COMMIT PREPARED fail with "could not read two-phase state from WAL".
Non-WAL PostgreSQL reads haven't been affected, likely because those readers
and writers have buffering systems in common.  See
https://postgr.es/m/20220116210241.GC756210@rfd.leadboat.com for details.

=cut

sub has_wal_read_bug
{
	return
		 $Config{osname} eq 'linux'
	  && $Config{archname} =~ /^sparc/
	  && !run_log([ qw(df -x ext4), $tmp_check ], '>', '/dev/null', '2>&1');
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
		if ($? == -1)
		{
			BAIL_OUT(
				sprintf(
					"failed to execute command \"%s\": $!", join(" ", @_)));
		}
		elsif ($? & 127)
		{
			BAIL_OUT(
				sprintf(
					"command \"%s\" died with signal %d",
					join(" ", @_),
					$? & 127));
		}
		else
		{
			BAIL_OUT(
				sprintf(
					"command \"%s\" exited with value %d",
					join(" ", @_),
					$? >> 8));
		}
	}
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

=item pump_until(proc, timeout, stream, until)

Pump until string is matched on the specified stream, or timeout occurs.

=cut

sub pump_until
{
	my ($proc, $timeout, $stream, $until) = @_;
	$proc->pump_nb();
	while (1)
	{
		last if $$stream =~ /$until/;
		if ($timeout->is_expired)
		{
			diag(
				"pump_until: timeout expired when searching for \"$until\" with stream: \"$$stream\""
			);
			return 0;
		}
		if (not $proc->pumpable())
		{
			diag(
				"pump_until: process terminated unexpectedly when searching for \"$until\" with stream: \"$$stream\""
			);
			return 0;
		}
		$proc->pump();
	}
	return 1;
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
	  or croak "could not opendir \"$dir\": $!";
	my @direntries = readdir $dh;
	closedir $dh;
	return @direntries;
}

=pod

=item slurp_file(filename [, $offset])

Return the full contents of the specified file, beginning from an
offset position if specified.

=cut

sub slurp_file
{
	my ($filename, $offset) = @_;
	local $/;
	my $contents;
	my $fh;

	# On windows open file using win32 APIs, to allow us to set the
	# FILE_SHARE_DELETE flag ("d" below), otherwise other accesses to the file
	# may fail.
	if ($Config{osname} ne 'MSWin32')
	{
		open($fh, '<', $filename)
		  or croak "could not read \"$filename\": $!";
	}
	else
	{
		my $fHandle = createFile($filename, "r", "rwd")
		  or croak "could not open \"$filename\": $^E";
		OsFHandleOpen($fh = IO::Handle->new(), $fHandle, 'r')
		  or croak "could not read \"$filename\": $^E\n";
	}

	if (defined($offset))
	{
		seek($fh, $offset, SEEK_SET)
		  or croak "could not seek \"$filename\": $!";
	}

	$contents = <$fh>;
	close $fh;

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
	  or croak "could not write \"$filename\": $!";
	print $fh $str;
	close $fh;
	return;
}

=pod

=item string_replace_file(filename, find, replace)

Find and replace string of a given file.

=cut

sub string_replace_file
{
	my ($filename, $find, $replace) = @_;
	open(my $in, '<', $filename) or croak $!;
	my $content = '';
	while (<$in>)
	{
		$_ =~ s/$find/$replace/;
		$content = $content . $_;
	}
	close $in;
	open(my $out, '>', $filename) or croak $!;
	print $out $content;
	close($out);

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
			wanted => sub {
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
					my $msg = "unable to stat $File::Find::name: $!";
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
			wanted => sub {
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

=item scan_server_header(header_path, regexp)

Returns an array that stores all the matches of the given regular expression
within the PostgreSQL installation's C<header_path>.  This can be used to
retrieve specific value patterns from the installation's header files.

=cut

sub scan_server_header
{
	my ($header_path, $regexp) = @_;

	my ($stdout, $stderr);
	my $result = IPC::Run::run [ 'pg_config', '--includedir-server' ], '>',
	  \$stdout, '2>', \$stderr
	  or die "could not execute pg_config";
	chomp($stdout);
	$stdout =~ s/\r$//;

	open my $header_h, '<', "$stdout/$header_path" or die "$!";

	my @match = undef;
	while (<$header_h>)
	{
		my $line = $_;

		if (@match = $line =~ /^$regexp/)
		{
			last;
		}
	}

	close $header_h;
	die "could not find match in header $header_path\n"
	  unless @match;
	return @match;
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

=item compare_files(file1, file2, testname)

Check that two files match, printing the difference if any.

C<line_comp_function> is an optional CODE reference to a line comparison
function, passed down as-is to File::Compare::compare_text.

=cut

sub compare_files
{
	my ($file1, $file2, $testname, $line_comp_function) = @_;

	# If nothing is given, all lines should be equal.
	$line_comp_function = sub { $_[0] ne $_[1] }
	  unless defined $line_comp_function;

	my $compare_res =
	  File::Compare::compare_text($file1, $file2, $line_comp_function);
	is($compare_res, 0, $testname);

	# Provide more context if the files do not match.
	if ($compare_res != 0)
	{
		my ($stdout, $stderr) =
		  run_command([ 'diff', '-u', $file1, $file2 ]);
		print "=== diff of $file1 and $file2\n";
		print "=== stdout ===\n";
		print $stdout;
		print "=== stderr ===\n";
		print $stderr;
		print "=== EOF ===\n";
	}

	return;
}

=pod

=item dir_symlink(oldname, newname)

Portably create a symlink for a directory. On Windows this creates a junction
point. Elsewhere it just calls perl's builtin symlink.

=cut

sub dir_symlink
{
	my $oldname = shift;
	my $newname = shift;
	if ($windows_os)
	{
		$oldname =~ s,/,\\,g;
		$newname =~ s,/,\\,g;
		my $cmd = qq{mklink /j "$newname" "$oldname"};
		if ($Config{osname} eq 'msys')
		{
			# need some indirection on msys
			$cmd = qq{echo '$cmd' | \$COMSPEC /Q};
		}
		system($cmd) == 0 or die;
	}
	else
	{
		symlink $oldname, $newname or die $!;
	}
	die "No $newname" unless -e $newname;
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

	# Normally, if the child called exit(N), IPC::Run::result() returns N.  On
	# Windows, with IPC::Run v20220807.0 and earlier, full_results() is the
	# method that returns N (https://github.com/toddr/IPC-Run/issues/161).
	my $result =
	  ($Config{osname} eq "MSWin32" && $IPC::Run::VERSION <= 20220807.0)
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

	# This value isn't set in stone, it reflects the current
	# convention in use.  Most output actually tries to aim for 80.
	my $max_line_length = 95;
	my @long_lines = grep { length > $max_line_length } split /\n/, $stdout;
	is(scalar @long_lines, 0, "$cmd --help maximum line length")
	  or diag("These lines are too long (>$max_line_length):\n",
		join("\n", @long_lines));

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
