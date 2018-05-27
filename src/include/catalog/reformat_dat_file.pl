#!/usr/bin/perl
#----------------------------------------------------------------------
#
# reformat_dat_file.pl
#    Perl script that reads in catalog data file(s) and writes out
#    functionally equivalent file(s) in a standard format.
#
#    In each entry of a reformatted file, metadata fields (if any) come
#    first, with normal attributes starting on the following line, in
#    the same order as the columns of the corresponding catalog.
#    Comments and blank lines are preserved.
#
# Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/include/catalog/reformat_dat_file.pl
#
#----------------------------------------------------------------------

use strict;
use warnings;

# If you copy this script to somewhere other than src/include/catalog,
# you'll need to modify this "use lib" or provide a suitable -I switch.
use FindBin;
use lib "$FindBin::RealBin/../../backend/catalog/";
use Catalog;

my @input_files;
my $output_path = '';
my $full_tuples = 0;

# Process command line switches.
while (@ARGV)
{
	my $arg = shift @ARGV;
	if ($arg !~ /^-/)
	{
		push @input_files, $arg;
	}
	elsif ($arg =~ /^-o/)
	{
		$output_path = length($arg) > 2 ? substr($arg, 2) : shift @ARGV;
	}
	elsif ($arg eq '--full-tuples')
	{
		$full_tuples = 1;
	}
	else
	{
		usage();
	}
}

# Sanity check arguments.
die "No input files.\n"
  if !@input_files;

# Make sure output_path ends in a slash.
if ($output_path ne '' && substr($output_path, -1) ne '/')
{
	$output_path .= '/';
}

# Metadata of a catalog entry
my @METADATA = ('oid', 'oid_symbol', 'descr');

# Read all the input files into internal data structures.
# We pass data file names as arguments and then look for matching
# headers to parse the schema from.
my %catalogs;
my %catalog_data;
my @catnames;
foreach my $datfile (@input_files)
{
	$datfile =~ /(.+)\.dat$/
	  or die "Input files need to be data (.dat) files.\n";

	my $header = "$1.h";
	die "There in no header file corresponding to $datfile"
	  if !-e $header;

	my $catalog = Catalog::ParseHeader($header);
	my $catname = $catalog->{catname};
	my $schema  = $catalog->{columns};

	push @catnames, $catname;
	$catalogs{$catname} = $catalog;

	$catalog_data{$catname} = Catalog::ParseData($datfile, $schema, 1);
}

########################################################################
# At this point, we have read all the data. If you are modifying this
# script for bulk editing, this is a good place to build lookup tables,
# if you need to. In the following example, the "next if !ref $row"
# check below is a hack to filter out non-hash objects. This is because
# we build the lookup tables from data that we read using the
# "preserve_formatting" parameter.
#
##Index access method lookup.
#my %amnames;
#foreach my $row (@{ $catalog_data{pg_am} })
#{
#	next if !ref $row;
#	$amnames{$row->{oid}} = $row->{amname};
#}
########################################################################

# Write the data.
foreach my $catname (@catnames)
{
	my $catalog = $catalogs{$catname};
	my @attnames;
	my $schema = $catalog->{columns};

	foreach my $column (@$schema)
	{
		my $attname = $column->{name};
		push @attnames, $attname;
	}

	# Overwrite .dat files in place, since they are under version control.
	my $datfile = "$output_path$catname.dat";
	open my $dat, '>', $datfile
	  or die "can't open $datfile: $!";

	foreach my $data (@{ $catalog_data{$catname} })
	{

		# Hash ref representing a data entry.
		if (ref $data eq 'HASH')
		{
			my %values = %$data;

			############################################################
			# At this point we have the full tuple in memory as a hash
			# and can do any operations we want. As written, it only
			# removes default values, but this script can be adapted to
			# do one-off bulk-editing.
			############################################################

			if (!$full_tuples)
			{
				strip_default_values(\%values, $schema, $catname);
			}

			print $dat "{";

			# Separate out metadata fields for readability.
			my $metadata_str = format_hash(\%values, @METADATA);
			if ($metadata_str)
			{
				print $dat $metadata_str;

				# User attributes start on next line.
				print $dat ",\n ";
			}

			my $data_str = format_hash(\%values, @attnames);
			print $dat $data_str;
			print $dat " },\n";
		}

		# Strings -- handle accordingly or ignore. It was necessary to
		# ignore bare commas during the initial data conversion. This
		# should be a no-op now, but we may as well keep that behavior.

		# Preserve blank lines.
		elsif ($data =~ /^\s*$/)
		{
			print $dat "\n";
		}

		# Preserve comments or brackets that are on their own line.
		elsif ($data =~ /^\s*(\[|\]|#.*?)\s*$/)
		{
			print $dat "$1\n";
		}
	}
	close $dat;
}

# Remove column values for which there is a matching default,
# or if the value can be computed from other columns.
sub strip_default_values
{
	my ($row, $schema, $catname) = @_;

	# Delete values that match defaults.
	foreach my $column (@$schema)
	{
		my $attname = $column->{name};
		die "strip_default_values: $catname.$attname undefined\n"
		  if !defined $row->{$attname};

		if (defined $column->{default}
			and ($row->{$attname} eq $column->{default}))
		{
			delete $row->{$attname};
		}
	}

	# Delete computed values.  See AddDefaultValues() in Catalog.pm.
	# Note: This must be done after deleting values matching defaults.
	if ($catname eq 'pg_proc')
	{
		delete $row->{pronargs} if defined $row->{proargtypes};
	}
	return;
}

# Format the individual elements of a Perl hash into a valid string
# representation. We do this ourselves, rather than use native Perl
# facilities, so we can keep control over the exact formatting of the
# data files.
sub format_hash
{
	my $data          = shift;
	my @orig_attnames = @_;

	# Copy attname to new array if it has a value, so we can determine
	# the last populated element. We do this because we may have default
	# values or empty metadata fields.
	my @attnames;
	foreach my $orig_attname (@orig_attnames)
	{
		push @attnames, $orig_attname
		  if defined $data->{$orig_attname};
	}

	# When calling this function, we ether have an open-bracket or a
	# leading space already.
	my $char_count = 1;

	my $threshold;
	my $hash_str      = '';
	my $element_count = 0;

	foreach my $attname (@attnames)
	{
		$element_count++;

		# To limit the line to 80 chars, we need to account for the
		# trailing characters.
		if ($element_count == $#attnames + 1)
		{
			# Last element, so allow space for ' },'
			$threshold = 77;
		}
		else
		{
			# Just need space for trailing comma
			$threshold = 79;
		}

		if ($element_count > 1)
		{
			$hash_str .= ',';
			$char_count++;
		}

		my $value = $data->{$attname};

		# Escape single quotes.
		$value =~ s/'/\\'/g;

		# Include a leading space in the key-value pair, since this will
		# always go after either a comma or an additional padding space on
		# the next line.
		my $element        = " $attname => '$value'";
		my $element_length = length($element);

		# If adding the element to the current line would expand the line
		# beyond 80 chars, put it on the next line. We don't do this for
		# the first element, since that would create a blank line.
		if ($element_count > 1 and $char_count + $element_length > $threshold)
		{

			# Put on next line with an additional space preceding. There
			# are now two spaces in front of the key-value pair, lining
			# it up with the line above it.
			$hash_str .= "\n $element";
			$char_count = $element_length + 1;
		}
		else
		{
			$hash_str .= $element;
			$char_count += $element_length;
		}
	}
	return $hash_str;
}

sub usage
{
	die <<EOM;
Usage: reformat_dat_file.pl [options] datafile...

Options:
    -o PATH          write output files to PATH instead of current directory
    --full-tuples    write out full tuples, including default values

Expects a list of .dat files as arguments.

EOM
}
