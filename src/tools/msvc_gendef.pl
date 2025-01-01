
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use List::Util qw(min);
use Getopt::Long;

#
# Script that generates a .DEF file for all objects in a directory
#
# src/tools/msvc_gendef.pl
#

# Given a symbol file path, loops over its contents
# and returns a list of symbols of interest as a dictionary
# of 'symbolname' -> symtype, where symtype is:
#
#     0    a CODE symbol, left undecorated in the .DEF
#     1    A DATA symbol, i.e. global var export
#
sub extract_syms
{
	my ($symfile, $def) = @_;
	open(my $f, '<', $symfile) || die "Could not open $symfile for $_: $!\n";
	while (<$f>)
	{

		# Expected symbol lines look like:
		#
		# 0   1        2      3            4            5 6
		# IDX SYMBOL   SECT   SYMTYPE      SYMSTATIC      SYMNAME
		# ------------------------------------------------------------------------
		# 02E 00000130 SECTA  notype       External     | _standbyState
		# 02F 00000009 SECT9  notype       Static       | _LocalRecoveryInProgress
		# 064 00000020 SECTC  notype ()    Static       | _XLogCheckBuffer
		# 065 00000000 UNDEF  notype ()    External     | _BufferGetTag
		#
		# See http://msdn.microsoft.com/en-us/library/b842y285.aspx
		#
		# We're not interested in the symbol index or offset.
		#
		# SECT[ION] is only examined to see whether the symbol is defined in a
		# COFF section of the local object file; if UNDEF, it's a symbol to be
		# resolved at link time from another object so we can't export it.
		#
		# SYMTYPE is always notype for C symbols as there's no typeinfo and no
		# way to get the symbol type from name (de)mangling. However, we care
		# if "notype" is suffixed by "()" or not. The presence of () means the
		# symbol is a function, the absence means it isn't.
		#
		# SYMSTATIC indicates whether it's a compilation-unit local "static"
		# symbol ("Static"), or whether it's available for use from other
		# compilation units ("External"). We export all symbols that aren't
		# static as part of the whole program DLL interface to produce UNIX-like
		# default linkage.
		#
		# SYMNAME is, obviously, the symbol name. The leading underscore
		# indicates that the _cdecl calling convention is used. See
		# http://www.unixwiz.net/techtips/win32-callconv.html
		# http://www.codeproject.com/Articles/1388/Calling-Conventions-Demystified
		#
		s/notype \(\)/func/g;
		s/notype/data/g;

		my @pieces = split;

		# Skip file and section headers and other non-symbol entries
		next unless defined($pieces[0]) and $pieces[0] =~ /^[A-F0-9]{3,}$/;

		# Skip blank symbol names
		next unless $pieces[6];

		# Skip externs used from another compilation unit
		next if ($pieces[2] eq "UNDEF");

		# Skip static symbols
		next unless ($pieces[4] eq "External");

		# Skip some more MSVC-generated crud
		next if $pieces[6] =~ /^@/;
		next if $pieces[6] =~ /^\(/;

		# __real and __xmm are out-of-line floating point literals and
		# (for __xmm) their SIMD equivalents. They shouldn't be part
		# of the DLL interface.
		next if $pieces[6] =~ /^__real/;
		next if $pieces[6] =~ /^__xmm/;

		# __imp entries are imports from other DLLs, eg __imp__malloc .
		# (We should never have one of these that hasn't already been skipped
		# by the UNDEF test above, though).
		next if $pieces[6] =~ /^__imp/;

		# More under-documented internal crud
		next if $pieces[6] =~ /NULL_THUNK_DATA$/;
		next if $pieces[6] =~ /^__IMPORT_DESCRIPTOR/;
		next if $pieces[6] =~ /^__NULL_IMPORT/;

		# Skip string literals
		next if $pieces[6] =~ /^\?\?_C/;

		# We assume that if a symbol is defined as data, then as a function,
		# the linker will reject the binary anyway. So it's OK to just pick
		# whatever came last.
		$def->{ $pieces[6] } = $pieces[3];
	}
	close($f);
	return;
}

sub writedef
{
	my ($deffile, $arch, $def) = @_;
	open(my $fh, '>', $deffile) || die "Could not write to $deffile\n";
	print $fh "EXPORTS\n";
	foreach my $f (sort keys %{$def})
	{
		my $isdata = $def->{$f} eq 'data';

		# Strip the leading underscore for win32, but not x64
		$f =~ s/^_//
		  unless ($arch eq "x86_64");

		# Emit just the name if it's a function symbol, or emit the name
		# decorated with the DATA option for variables.
		if ($isdata)
		{
			print $fh "  $f DATA\n";
		}
		else
		{
			print $fh "  $f\n";
		}
	}
	close($fh);
	return;
}


sub usage
{
	die("Usage: msvc_gendef.pl --arch <arch> --deffile <deffile> --tempdir <tempdir> files-or-directories\n"
		  . "    arch: x86 | x86_64\n"
		  . "    deffile: path of the generated file\n"
		  . "    tempdir: directory for temporary files\n"
		  . "    files or directories: object files or directory containing object files\n"
	);
}

my $arch;
my $deffile;
my $tempdir = '.';

GetOptions(
	'arch:s' => \$arch,
	'deffile:s' => \$deffile,
	'tempdir:s' => \$tempdir,) or usage();

usage("arch: $arch")
  unless ($arch eq 'x86' || $arch eq 'x86_64');

my @files;

foreach my $in (@ARGV)
{
	if (-d $in)
	{
		push @files, glob "$in/*.obj";
	}
	else
	{
		push @files, $in;
	}
}

# if the def file exists and is newer than all input object files, skip
# its creation
if (-f $deffile
	&& (-M $deffile < min(map { -M } @files)))
{
	print "Not re-generating $deffile, file already exists.\n";
	exit(0);
}

print "Generating $deffile in tempdir $tempdir\n";

my %def = ();

my $symfile = "$tempdir/all.sym";
my $tmpfile = "$tempdir/tmp.sym";
mkdir($tempdir) unless -d $tempdir;

my $cmd = "dumpbin /nologo /symbols /out:$tmpfile " . join(' ', @files);

system($cmd) == 0 or die "Could not call dumpbin";
rename($tmpfile, $symfile) or die $!;
extract_syms($symfile, \%def);
print "\n";

writedef($deffile, $arch, \%def);

print "Generated " . scalar(keys(%def)) . " symbols\n";
