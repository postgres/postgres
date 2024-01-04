#!/usr/bin/perl
#----------------------------------------------------------------------
#
# renumber_oids.pl
#    Perl script that shifts a range of OIDs in the Postgres catalog data
#    to a different range, skipping any OIDs that are already in use.
#
#    Note: This does not reformat the .dat files, so you may want
#    to run reformat_dat_file.pl afterwards.
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/include/catalog/renumber_oids.pl
#
#----------------------------------------------------------------------

use strict;
use warnings FATAL => 'all';

use FindBin;
use Getopt::Long;

# Must run in src/include/catalog
chdir $FindBin::RealBin or die "could not cd to $FindBin::RealBin: $!\n";

use lib "$FindBin::RealBin/../../backend/catalog/";
use Catalog;

# We'll need this number.
my $FirstGenbkiObjectId =
  Catalog::FindDefinedSymbol('access/transam.h', '..', 'FirstGenbkiObjectId');

# Process command line switches.
my $output_path = '';
my $first_mapped_oid = 0;
my $last_mapped_oid = $FirstGenbkiObjectId - 1;
my $target_oid = 0;

GetOptions(
	'output=s' => \$output_path,
	'first-mapped-oid=i' => \$first_mapped_oid,
	'last-mapped-oid=i' => \$last_mapped_oid,
	'target-oid=i' => \$target_oid) || usage();

# Sanity check arguments.
die "Unexpected non-switch arguments.\n" if @ARGV;
die "--first-mapped-oid must be specified.\n"
  if $first_mapped_oid <= 0;
die "Empty mapped OID range.\n"
  if $last_mapped_oid < $first_mapped_oid;
die "--target-oid must be specified.\n"
  if $target_oid <= 0;
die "--target-oid must not be within mapped OID range.\n"
  if $target_oid >= $first_mapped_oid && $target_oid <= $last_mapped_oid;

# Make sure output_path ends in a slash.
if ($output_path ne '' && substr($output_path, -1) ne '/')
{
	$output_path .= '/';
}

# Collect all the existing assigned OIDs (including those to be remapped).
my @header_files = glob("pg_*.h");
my $oids = Catalog::FindAllOidsFromHeaders(@header_files);

# Hash-ify the existing OIDs for convenient lookup.
my %oidhash;
@oidhash{@$oids} = undef;

# Select new OIDs for existing OIDs in the mapped range.
# We do this first so that we preserve the ordering of the mapped OIDs
# (for reproducibility's sake), and so that if we fail due to running out
# of OID room, that happens before we've overwritten any files.
my %maphash;
my $next_oid = $target_oid;

for (
	my $mapped_oid = $first_mapped_oid;
	$mapped_oid <= $last_mapped_oid;
	$mapped_oid++)
{
	next if !exists $oidhash{$mapped_oid};
	$next_oid++
	  while (
		exists $oidhash{$next_oid}
		|| (   $next_oid >= $first_mapped_oid
			&& $next_oid <= $last_mapped_oid));
	die "Reached FirstGenbkiObjectId before assigning all OIDs.\n"
	  if $next_oid >= $FirstGenbkiObjectId;
	$maphash{$mapped_oid} = $next_oid;
	$next_oid++;
}

die "There are no OIDs in the mapped range.\n" if $next_oid == $target_oid;

# Read each .h file and write out modified data.
foreach my $input_file (@header_files)
{
	$input_file =~ /(\w+)\.h$/
	  or die "Input file $input_file needs to be a .h file.\n";
	my $catname = $1;

	# Ignore generated *_d.h files.
	next if $catname =~ /_d$/;

	open(my $ifd, '<', $input_file) || die "$input_file: $!";

	# Write output files to specified directory.
	# Use a .tmp suffix, then rename into place, in case we're overwriting.
	my $output_file = "$output_path$catname.h";
	my $tmp_output_file = "$output_file.tmp";
	open my $ofd, '>', $tmp_output_file
	  or die "can't open $tmp_output_file: $!";
	my $changed = 0;

	# Scan the input file.
	while (<$ifd>)
	{
		my $line = $_;

		# Check for OID-defining macros that Catalog::ParseHeader knows about,
		# and update OIDs as needed.
		if ($line =~ m/^(DECLARE_TOAST\(\s*\w+,\s*)(\d+)(,\s*)(\d+)\)/)
		{
			my $oid2 = $2;
			my $oid4 = $4;
			if (exists $maphash{$oid2})
			{
				$oid2 = $maphash{$oid2};
				my $repl = $1 . $oid2 . $3 . $oid4 . ")";
				$line =~ s/^DECLARE_TOAST\(\s*\w+,\s*\d+,\s*\d+\)/$repl/;
				$changed = 1;
			}
			if (exists $maphash{$oid4})
			{
				$oid4 = $maphash{$oid4};
				my $repl = $1 . $oid2 . $3 . $oid4 . ")";
				$line =~ s/^DECLARE_TOAST\(\s*\w+,\s*\d+,\s*\d+\)/$repl/;
				$changed = 1;
			}
		}
		elsif ($line =~
			m/^(DECLARE_TOAST_WITH_MACRO\(\s*\w+,\s*)(\d+)(,\s*)(\d+)(,\s*\w+,\s*\w+)\)/
		  )
		{
			my $oid2 = $2;
			my $oid4 = $4;
			if (exists $maphash{$oid2})
			{
				$oid2 = $maphash{$oid2};
				my $repl = $1 . $oid2 . $3 . $oid4 . $5 . ")";
				$line =~
				  s/^DECLARE_TOAST_WITH_MACRO\(\s*\w+,\s*\d+,\s*\d+,\s*\w+,\s*\w+\)/$repl/;
				$changed = 1;
			}
			if (exists $maphash{$oid4})
			{
				$oid4 = $maphash{$oid4};
				my $repl = $1 . $oid2 . $3 . $oid4 . $5 . ")";
				$line =~
				  s/^DECLARE_TOAST_WITH_MACRO\(\s*\w+,\s*\d+,\s*\d+,\s*\w+,\s*\w+\)/$repl/;
				$changed = 1;
			}
		}
		elsif ($line =~
			m/^(DECLARE_(UNIQUE_)?INDEX(_PKEY)?\(\s*\w+,\s*)(\d+)(,\s*.+)\)/)
		{
			if (exists $maphash{$4})
			{
				my $repl = $1 . $maphash{$4} . $5 . ")";
				$line =~
				  s/^DECLARE_(UNIQUE_)?INDEX(_PKEY)?\(\s*\w+,\s*\d+,\s*.+\)/$repl/;
				$changed = 1;
			}
		}
		elsif (/^(DECLARE_OID_DEFINING_MACRO\(\s*\w+,\s*)(\d+)\)/)
		{
			if (exists $maphash{$2})
			{
				my $repl = $1 . $maphash{$2} . ")";
				$line =~
				  s/^DECLARE_OID_DEFINING_MACRO\(\s*\w+,\s*\d+\)/$repl/;
				$changed = 1;
			}
		}
		elsif ($line =~ m/^CATALOG\((\w+),(\d+),(\w+)\)/)
		{
			if (exists $maphash{$2})
			{
				my $repl =
				  "CATALOG(" . $1 . "," . $maphash{$2} . "," . $3 . ")";
				$line =~ s/^CATALOG\(\w+,\d+,\w+\)/$repl/;
				$changed = 1;
			}

			if ($line =~ m/BKI_ROWTYPE_OID\((\d+),(\w+)\)/)
			{
				if (exists $maphash{$1})
				{
					my $repl =
					  "BKI_ROWTYPE_OID(" . $maphash{$1} . "," . $2 . ")";
					$line =~ s/BKI_ROWTYPE_OID\(\d+,\w+\)/$repl/;
					$changed = 1;
				}
			}
		}

		print $ofd $line;
	}

	close $ifd;
	close $ofd;

	# Avoid updating files if we didn't change them.
	if ($changed || $output_path ne '')
	{
		rename $tmp_output_file, $output_file
		  or die "can't rename $tmp_output_file to $output_file: $!";
	}
	else
	{
		unlink $tmp_output_file
		  or die "can't unlink $tmp_output_file: $!";
	}
}

# Likewise, read each .dat file and write out modified data.
foreach my $input_file (glob("pg_*.dat"))
{
	$input_file =~ /(\w+)\.dat$/
	  or die "Input file $input_file needs to be a .dat file.\n";
	my $catname = $1;

	open(my $ifd, '<', $input_file) || die "$input_file: $!";

	# Write output files to specified directory.
	# Use a .tmp suffix, then rename into place, in case we're overwriting.
	my $output_file = "$output_path$catname.dat";
	my $tmp_output_file = "$output_file.tmp";
	open my $ofd, '>', $tmp_output_file
	  or die "can't open $tmp_output_file: $!";
	my $changed = 0;

	# Scan the input file.
	while (<$ifd>)
	{
		my $line = $_;

		# Check for oid => 'nnnn', and replace if within mapped range.
		if ($line =~ m/\b(oid\s*=>\s*)'(\d+)'/)
		{
			if (exists $maphash{$2})
			{
				my $repl = $1 . "'" . $maphash{$2} . "'";
				$line =~ s/\boid\s*=>\s*'\d+'/$repl/;
				$changed = 1;
			}
		}

		# Likewise for array_type_oid.
		if ($line =~ m/\b(array_type_oid\s*=>\s*)'(\d+)'/)
		{
			if (exists $maphash{$2})
			{
				my $repl = $1 . "'" . $maphash{$2} . "'";
				$line =~ s/\barray_type_oid\s*=>\s*'\d+'/$repl/;
				$changed = 1;
			}
		}

		print $ofd $line;
	}

	close $ifd;
	close $ofd;

	# Avoid updating files if we didn't change them.
	if ($changed || $output_path ne '')
	{
		rename $tmp_output_file, $output_file
		  or die "can't rename $tmp_output_file to $output_file: $!";
	}
	else
	{
		unlink $tmp_output_file
		  or die "can't unlink $tmp_output_file: $!";
	}
}

sub usage
{
	my $last = $FirstGenbkiObjectId - 1;
	die <<EOM;
Usage: renumber_oids.pl [--output PATH] --first-mapped-oid X [--last-mapped-oid Y] --target-oid Z

Options:
    --output PATH           output directory (default '.')
    --first-mapped-oid X    first OID to be moved
    --last-mapped-oid Y     last OID to be moved (default $last)
    --target-oid Z          first OID to move to

Catalog *.h and *.dat files are updated and written to the
output directory; by default, this overwrites the input files.

Caution: the output PATH will be interpreted relative to
src/include/catalog, even if you start the script
in some other directory.

EOM
}
