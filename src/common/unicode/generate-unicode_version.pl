#!/usr/bin/perl
#
# Generate header file with Unicode version used by Postgres.
#
# Output: unicode_version.h
#
# Copyright (c) 2000-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use Getopt::Long;

use FindBin;
use lib "$FindBin::RealBin/../../tools/";

my $output_path = '.';
my $version_str = undef;

GetOptions('outdir:s' => \$output_path, 'version:s' => \$version_str);

my @version_parts = split /\./, $version_str;

my $unicode_version_str = sprintf "%d.%d", $version_parts[0],
  $version_parts[1];

my $output_file = "$output_path/unicode_version.h";

# Start writing out the output files
open my $OT, '>', $output_file
  or die "Could not open output file $output_file: $!\n";

print $OT <<HEADER;
/*-------------------------------------------------------------------------
 *
 * unicode_version.h
 *	  Unicode version used by Postgres.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/unicode_version.h
 *
 *-------------------------------------------------------------------------
 */

#define PG_UNICODE_VERSION		"$unicode_version_str"
HEADER
