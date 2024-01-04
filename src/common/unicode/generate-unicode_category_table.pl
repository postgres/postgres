#!/usr/bin/perl
#
# Generate a code point category table and its lookup utilities, using
# Unicode data files as input.
#
# Input: UnicodeData.txt
# Output: unicode_category_table.h
#
# Copyright (c) 2000-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use Getopt::Long;

use FindBin;
use lib "$FindBin::RealBin/../../tools/";

my $CATEGORY_UNASSIGNED = 'Cn';

my $output_path = '.';

GetOptions('outdir:s' => \$output_path);

my $output_table_file = "$output_path/unicode_category_table.h";

my $FH;

# Read entries from UnicodeData.txt into a list of codepoint ranges
# and their general category.
my @category_ranges = ();
my $range_start = undef;
my $range_end = undef;
my $range_category = undef;

# If between a "<..., First>" entry and a "<..., Last>" entry, the gap in
# codepoints represents a range, and $gap_category is equal to the
# category for both (which must match). Otherwise, the gap represents
# unassigned code points.
my $gap_category = undef;

open($FH, '<', "$output_path/UnicodeData.txt")
  or die "Could not open $output_path/UnicodeData.txt: $!.";
while (my $line = <$FH>)
{
	my @elts = split(';', $line);
	my $code = hex($elts[0]);
	my $name = $elts[1];
	my $category = $elts[2];

	die "codepoint out of range" if $code > 0x10FFFF;
	die "unassigned codepoint in UnicodeData.txt" if $category eq $CATEGORY_UNASSIGNED;

	if (!defined($range_start)) {
		my $code_str = sprintf "0x%06x", $code;
		die if defined($range_end) || defined($range_category) || defined($gap_category);
		die "unexpected first entry <..., Last>" if ($name =~ /Last>/);
		die "expected 0x000000 for first entry, got $code_str" if $code != 0x000000;

		# initialize
		$range_start = $code;
		$range_end = $code;
		$range_category = $category;
		if ($name =~ /<.*, First>$/) {
			$gap_category = $category;
		} else {
			$gap_category = $CATEGORY_UNASSIGNED;
		}
		next;
	}

	# Gap in codepoints detected. If it's a different category than
	# the current range, emit the current range and initialize a new
	# range representing the gap.
	if ($range_end + 1 != $code && $range_category ne $gap_category) {
		if ($range_category ne $CATEGORY_UNASSIGNED) {
			push(@category_ranges, {start => $range_start, end => $range_end,
									category => $range_category});
		}
		$range_start = $range_end + 1;
		$range_end = $code - 1;
		$range_category = $gap_category;
	}

	# different category; new range
	if ($range_category ne $category) {
		if ($range_category ne $CATEGORY_UNASSIGNED) {
			push(@category_ranges, {start => $range_start, end => $range_end,
									category => $range_category});
		}
		$range_start = $code;
		$range_end = $code;
		$range_category = $category;
	}

	if ($name =~ /<.*, First>$/) {
		die "<..., First> entry unexpectedly follows another <..., First> entry"
		  if $gap_category ne $CATEGORY_UNASSIGNED;
		$gap_category = $category;
	}
	elsif ($name =~ /<.*, Last>$/) {
		die "<..., First> and <..., Last> entries have mismatching general category"
		  if $gap_category ne $category;
		$gap_category = $CATEGORY_UNASSIGNED;
	}
	else {
		die "unexpected entry found between <..., First> and <..., Last>"
		  if $gap_category ne $CATEGORY_UNASSIGNED;
	}

	$range_end = $code;
}
close $FH;

die "<..., First> entry with no corresponding <..., Last> entry"
  if $gap_category ne $CATEGORY_UNASSIGNED;

# emit final range
if ($range_category ne $CATEGORY_UNASSIGNED) {
	push(@category_ranges, {start => $range_start, end => $range_end,
							category => $range_category});
}

my $num_ranges = scalar @category_ranges;

# See: https://www.unicode.org/reports/tr44/#General_Category_Values
my $categories = {
	Cn => 'PG_U_UNASSIGNED',
	Lu => 'PG_U_UPPERCASE_LETTER',
	Ll => 'PG_U_LOWERCASE_LETTER',
	Lt => 'PG_U_TITLECASE_LETTER',
	Lm => 'PG_U_MODIFIER_LETTER',
	Lo => 'PG_U_OTHER_LETTER',
	Mn => 'PG_U_NONSPACING_MARK',
	Me => 'PG_U_ENCLOSING_MARK',
	Mc => 'PG_U_SPACING_MARK',
	Nd => 'PG_U_DECIMAL_NUMBER',
	Nl => 'PG_U_LETTER_NUMBER',
	No => 'PG_U_OTHER_NUMBER',
	Zs => 'PG_U_SPACE_SEPARATOR',
	Zl => 'PG_U_LINE_SEPARATOR',
	Zp => 'PG_U_PARAGRAPH_SEPARATOR',
	Cc => 'PG_U_CONTROL',
	Cf => 'PG_U_FORMAT',
	Co => 'PG_U_PRIVATE_USE',
	Cs => 'PG_U_SURROGATE',
	Pd => 'PG_U_DASH_PUNCTUATION',
	Ps => 'PG_U_OPEN_PUNCTUATION',
	Pe => 'PG_U_CLOSE_PUNCTUATION',
	Pc => 'PG_U_CONNECTOR_PUNCTUATION',
	Po => 'PG_U_OTHER_PUNCTUATION',
	Sm => 'PG_U_MATH_SYMBOL',
	Sc => 'PG_U_CURRENCY_SYMBOL',
	Sk => 'PG_U_MODIFIER_SYMBOL',
	So => 'PG_U_OTHER_SYMBOL',
	Pi => 'PG_U_INITIAL_PUNCTUATION',
	Pf => 'PG_U_FINAL_PUNCTUATION'
};

# Start writing out the output files
open my $OT, '>', $output_table_file
  or die "Could not open output file $output_table_file: $!\n";

print $OT <<HEADER;
/*-------------------------------------------------------------------------
 *
 * unicode_category_table.h
 *	  Category table for Unicode character classification.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/unicode_category_table.h
 *
 *-------------------------------------------------------------------------
 */

#include "common/unicode_category.h"

/*
 * File auto-generated by src/common/unicode/generate-unicode_category_table.pl,
 * do not edit. There is deliberately not an #ifndef PG_UNICODE_CATEGORY_TABLE_H
 * here.
 */
typedef struct
{
	uint32		first;			/* Unicode codepoint */
	uint32		last;			/* Unicode codepoint */
	uint8		category;		/* General Category */
}			pg_category_range;

/* table of Unicode codepoint ranges and their categories */
static const pg_category_range unicode_categories[$num_ranges] =
{
HEADER

my $firsttime = 1;
foreach my $range (@category_ranges) {
	printf $OT ",\n" unless $firsttime;
	$firsttime = 0;

	my $category = $categories->{$range->{category}};
	die "category missing: $range->{category}" unless $category;
	printf $OT "\t{0x%06x, 0x%06x, %s}", $range->{start}, $range->{end}, $category;
}
print $OT "\n};\n";
