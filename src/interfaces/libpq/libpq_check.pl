#!/usr/bin/perl
#
# src/interfaces/libpq/libpq_check.pl
#
# Copyright (c) 2025-2026, PostgreSQL Global Development Group
#
# Check the state of a libpq library.  Currently, this script checks that
# exit() is not called, because client libraries must not terminate the
# host application.
#
# This script is called by both Makefile and Meson.

use strict;
use warnings FATAL => 'all';

use Getopt::Long;
use Config;

my $nm_path;
my $input_file;
my $stamp_file;
my @problematic_lines;

Getopt::Long::GetOptions(
	'nm:s' => \$nm_path,
	'input_file:s' => \$input_file,
	'stamp_file:s' => \$stamp_file) or die "$0: wrong arguments\n";

die "$0: --input_file must be specified\n" unless defined $input_file;
die "$0: --nm must be specified\n" unless defined $nm_path and -x $nm_path;

sub create_stamp_file
{
	if (!(-f $stamp_file))
	{
		open my $fh, '>', $stamp_file
		  or die "can't open $stamp_file: $!";
		close $fh;
	}
}

# Skip on Windows and Solaris
if (   $Config{osname} =~ /MSWin32|cygwin|msys/i
	|| $Config{osname} =~ /solaris/i)
{
	exit 0;
}

# Run nm to scan for symbols.  If nm fails at runtime, skip the check.
open my $fh, '-|', "$nm_path -A -u $input_file 2>/dev/null"
  or exit 0;

while (<$fh>)
{
	# Set of symbols allowed.

	# The exclusion of __cxa_atexit is necessary on OpenBSD, which seems
	# to insert references to that even in pure C code.
	next if /__cxa_atexit/;

	# Excluding __tsan_func_exit is necessary when using ThreadSanitizer data
	# race detector which uses this function for instrumentation of function
	# exit.
	next if /__tsan_func_exit/;

	# Excluding pthread_exit allows legitimate thread terminations in some
	# builds.
	next if /pthread_exit/;

	# Anything containing "exit" is suspicious.
	# (Ideally we should reject abort() too, but there are various scenarios
	# where build toolchains insert abort() calls, e.g. to implement
	# assert().)
	if (/exit/)
	{
		push @problematic_lines, $_;
	}
}
close $fh;

if (@problematic_lines)
{
	print "libpq must not be calling any function which invokes exit\n";
	print "Problematic symbol references:\n";
	print @problematic_lines;

	exit 1;
}
# Create stamp file, if required
if (defined($stamp_file))
{
	create_stamp_file();
}

exit 0;
