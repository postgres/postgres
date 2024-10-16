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

# create a table of all codepoints < 0x80 and their associated
# categories and properties for fast lookups
my %opt_ascii = ();

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
	die "unassigned codepoint in UnicodeData.txt"
	  if $category eq $CATEGORY_UNASSIGNED;

	if ($code < 0x80)
	{
		my @properties = ();
		# No ASCII characters have category Titlecase_Letter,
		# but include here for completeness.
		push @properties, "PG_U_PROP_CASED" if ($category eq 'Lt');
		$opt_ascii{$code} = {
			Category => $category,
			Properties => \@properties
		};
	}

	if (!defined($range_start))
	{
		my $code_str = sprintf "0x%06x", $code;
		die
		  if defined($range_end)
		  || defined($range_category)
		  || defined($gap_category);
		die "unexpected first entry <..., Last>" if ($name =~ /Last>/);
		die "expected 0x000000 for first entry, got $code_str"
		  if $code != 0x000000;

		# initialize
		$range_start = $code;
		$range_end = $code;
		$range_category = $category;
		if ($name =~ /<.*, First>$/)
		{
			$gap_category = $category;
		}
		else
		{
			$gap_category = $CATEGORY_UNASSIGNED;
		}
		next;
	}

	# Gap in codepoints detected. If it's a different category than
	# the current range, emit the current range and initialize a new
	# range representing the gap.
	if ($range_end + 1 != $code && $range_category ne $gap_category)
	{
		if ($range_category ne $CATEGORY_UNASSIGNED)
		{
			push(
				@category_ranges,
				{
					start => $range_start,
					end => $range_end,
					category => $range_category
				});
		}
		$range_start = $range_end + 1;
		$range_end = $code - 1;
		$range_category = $gap_category;
	}

	# different category; new range
	if ($range_category ne $category)
	{
		if ($range_category ne $CATEGORY_UNASSIGNED)
		{
			push(
				@category_ranges,
				{
					start => $range_start,
					end => $range_end,
					category => $range_category
				});
		}
		$range_start = $code;
		$range_end = $code;
		$range_category = $category;
	}

	if ($name =~ /<.*, First>$/)
	{
		die
		  "<..., First> entry unexpectedly follows another <..., First> entry"
		  if $gap_category ne $CATEGORY_UNASSIGNED;
		$gap_category = $category;
	}
	elsif ($name =~ /<.*, Last>$/)
	{
		die
		  "<..., First> and <..., Last> entries have mismatching general category"
		  if $gap_category ne $category;
		$gap_category = $CATEGORY_UNASSIGNED;
	}
	else
	{
		die "unexpected entry found between <..., First> and <..., Last>"
		  if $gap_category ne $CATEGORY_UNASSIGNED;
	}

	$range_end = $code;
}
close $FH;

die "<..., First> entry with no corresponding <..., Last> entry"
  if $gap_category ne $CATEGORY_UNASSIGNED;

# emit final range
if ($range_category ne $CATEGORY_UNASSIGNED)
{
	push(
		@category_ranges,
		{
			start => $range_start,
			end => $range_end,
			category => $range_category
		});
}

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

# Find White_Space and Hex_Digit characters
my @white_space = ();
my @hex_digits = ();
my @join_control = ();
open($FH, '<', "$output_path/PropList.txt")
  or die "Could not open $output_path/PropList.txt: $!.";
while (my $line = <$FH>)
{
	my $pattern = qr/([0-9A-F\.]+)\s*;\s*(\w+)\s*#.*/s;
	next unless $line =~ $pattern;

	my $code = $line =~ s/$pattern/$1/rg;
	my $property = $line =~ s/$pattern/$2/rg;
	my $start;
	my $end;

	if ($code =~ /\.\./)
	{
		# code range
		my @sp = split /\.\./, $code;
		$start = hex($sp[0]);
		$end = hex($sp[1]);
	}
	else
	{
		# single code point
		$start = hex($code);
		$end = hex($code);
	}

	if ($property eq "White_Space")
	{
		push @white_space, { start => $start, end => $end };
		for (my $i = $start; $i <= $end && $i < 0x80; $i++)
		{
			push @{ $opt_ascii{$i}{Properties} }, "PG_U_PROP_WHITE_SPACE";
		}
	}
	elsif ($property eq "Hex_Digit")
	{
		push @hex_digits, { start => $start, end => $end };
		for (my $i = $start; $i <= $end && $i < 0x80; $i++)
		{
			push @{ $opt_ascii{$i}{Properties} }, "PG_U_PROP_HEX_DIGIT";
		}
	}
	elsif ($property eq "Join_Control")
	{
		push @join_control, { start => $start, end => $end };
		for (my $i = $start; $i <= $end && $i < 0x80; $i++)
		{
			push @{ $opt_ascii{$i}{Properties} }, "PG_U_PROP_JOIN_CONTROL";
		}
	}
}

# Find Alphabetic, Lowercase, and Uppercase characters
my @alphabetic = ();
my @lowercase = ();
my @uppercase = ();
my @case_ignorable = ();
open($FH, '<', "$output_path/DerivedCoreProperties.txt")
  or die "Could not open $output_path/DerivedCoreProperties.txt: $!.";
while (my $line = <$FH>)
{
	my $pattern = qr/^([0-9A-F\.]+)\s*;\s*(\w+)\s*#.*$/s;
	next unless $line =~ $pattern;

	my $code = $line =~ s/$pattern/$1/rg;
	my $property = $line =~ s/$pattern/$2/rg;
	my $start;
	my $end;

	if ($code =~ /\.\./)
	{
		# code range
		my @sp = split /\.\./, $code;
		die "line: {$line} code: {$code} sp[0] {$sp[0]} sp[1] {$sp[1]}"
		  unless $sp[0] =~ /^[0-9A-F]+$/ && $sp[1] =~ /^[0-9A-F]+$/;
		$start = hex($sp[0]);
		$end = hex($sp[1]);
	}
	else
	{
		die "line: {$line} code: {$code}" unless $code =~ /^[0-9A-F]+$/;
		# single code point
		$start = hex($code);
		$end = hex($code);
	}

	if ($property eq "Alphabetic")
	{
		push @alphabetic, { start => $start, end => $end };
		for (my $i = $start; $i <= $end && $i < 0x80; $i++)
		{
			push @{ $opt_ascii{$i}{Properties} }, "PG_U_PROP_ALPHABETIC";
		}
	}
	elsif ($property eq "Lowercase")
	{
		push @lowercase, { start => $start, end => $end };
		for (my $i = $start; $i <= $end && $i < 0x80; $i++)
		{
			push @{ $opt_ascii{$i}{Properties} }, "PG_U_PROP_LOWERCASE";
			push @{ $opt_ascii{$i}{Properties} }, "PG_U_PROP_CASED";
		}
	}
	elsif ($property eq "Uppercase")
	{
		push @uppercase, { start => $start, end => $end };
		for (my $i = $start; $i <= $end && $i < 0x80; $i++)
		{
			push @{ $opt_ascii{$i}{Properties} }, "PG_U_PROP_UPPERCASE";
			push @{ $opt_ascii{$i}{Properties} }, "PG_U_PROP_CASED";
		}
	}
	elsif ($property eq "Case_Ignorable")
	{
		push @case_ignorable, { start => $start, end => $end };
		for (my $i = $start; $i <= $end && $i < 0x80; $i++)
		{
			push @{ $opt_ascii{$i}{Properties} }, "PG_U_PROP_CASE_IGNORABLE";
		}
	}
}

my $num_category_ranges = scalar @category_ranges;
my $num_alphabetic_ranges = scalar @alphabetic;
my $num_lowercase_ranges = scalar @lowercase;
my $num_uppercase_ranges = scalar @uppercase;
my $num_case_ignorable_ranges = scalar @case_ignorable;
my $num_white_space_ranges = scalar @white_space;
my $num_hex_digit_ranges = scalar @hex_digits;
my $num_join_control_ranges = scalar @join_control;

# Start writing out the output file
open my $OT, '>', $output_table_file
  or die "Could not open output file $output_table_file: $!\n";

print $OT <<"EOS";
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
} pg_category_range;

typedef struct
{
	uint32		first;			/* Unicode codepoint */
	uint32		last;			/* Unicode codepoint */
} pg_unicode_range;

typedef struct
{
	uint8		category;
	uint8		properties;
} pg_unicode_properties;

/*
 * The properties currently used, in no particular order. Fits in a uint8, but
 * if more properties are added, a wider integer will be needed.
 */
#define PG_U_PROP_ALPHABETIC		(1 << 0)
#define PG_U_PROP_LOWERCASE			(1 << 1)
#define PG_U_PROP_UPPERCASE			(1 << 2)
#define PG_U_PROP_CASED				(1 << 3)
#define PG_U_PROP_CASE_IGNORABLE	(1 << 4)
#define PG_U_PROP_WHITE_SPACE		(1 << 5)
#define PG_U_PROP_JOIN_CONTROL		(1 << 6)
#define PG_U_PROP_HEX_DIGIT			(1 << 7)

EOS

print $OT <<"EOS";
/* table for fast lookup of ASCII codepoints */
static const pg_unicode_properties unicode_opt_ascii[128] =
{
EOS

for (my $i = 0; $i < 128; $i++)
{
	my $category_str = $categories->{ $opt_ascii{$i}->{Category} };
	my $props_str = (join ' | ', @{ $opt_ascii{$i}{Properties} }) || "0";
	printf $OT
	  "\t{\n\t\t/* 0x%06x */\n\t\t.category = %s,\n\t\t.properties = %s\n\t},\n",
	  $i, $category_str, $props_str;
}

print $OT "};\n\n";

print $OT <<"EOS";
/* table of Unicode codepoint ranges and their categories */
static const pg_category_range unicode_categories[$num_category_ranges] =
{
EOS

foreach my $range (@category_ranges)
{
	my $category = $categories->{ $range->{category} };
	die "category missing: $range->{category}" unless $category;
	printf $OT "\t{0x%06x, 0x%06x, %s},\n", $range->{start}, $range->{end},
	  $category;
}

print $OT "};\n\n";

print $OT <<"EOS";
/* table of Unicode codepoint ranges of Alphabetic characters */
static const pg_unicode_range unicode_alphabetic[$num_alphabetic_ranges] =
{
EOS

foreach my $range (@alphabetic)
{
	printf $OT "\t{0x%06x, 0x%06x},\n", $range->{start}, $range->{end};
}

print $OT "};\n\n";

print $OT <<"EOS";
/* table of Unicode codepoint ranges of Lowercase characters */
static const pg_unicode_range unicode_lowercase[$num_lowercase_ranges] =
{
EOS

foreach my $range (@lowercase)
{
	printf $OT "\t{0x%06x, 0x%06x},\n", $range->{start}, $range->{end};
}

print $OT "};\n\n";

print $OT <<"EOS";
/* table of Unicode codepoint ranges of Uppercase characters */
static const pg_unicode_range unicode_uppercase[$num_uppercase_ranges] =
{
EOS

foreach my $range (@uppercase)
{
	printf $OT "\t{0x%06x, 0x%06x},\n", $range->{start}, $range->{end};
}

print $OT "};\n\n";

print $OT <<"EOS";
/* table of Unicode codepoint ranges of Case_Ignorable characters */
static const pg_unicode_range unicode_case_ignorable[$num_case_ignorable_ranges] =
{
EOS

foreach my $range (@case_ignorable)
{
	printf $OT "\t{0x%06x, 0x%06x},\n", $range->{start}, $range->{end};
}

print $OT "};\n\n";

print $OT <<"EOS";
/* table of Unicode codepoint ranges of White_Space characters */
static const pg_unicode_range unicode_white_space[$num_white_space_ranges] =
{
EOS

foreach my $range (@white_space)
{
	printf $OT "\t{0x%06x, 0x%06x},\n", $range->{start}, $range->{end};
}

print $OT "};\n\n";

print $OT <<"EOS";
/* table of Unicode codepoint ranges of Hex_Digit characters */
static const pg_unicode_range unicode_hex_digit[$num_hex_digit_ranges] =
{
EOS

foreach my $range (@hex_digits)
{
	printf $OT "\t{0x%06x, 0x%06x},\n", $range->{start}, $range->{end};
}

print $OT "};\n\n";

print $OT <<"EOS";
/* table of Unicode codepoint ranges of Join_Control characters */
static const pg_unicode_range unicode_join_control[$num_join_control_ranges] =
{
EOS

foreach my $range (@join_control)
{
	printf $OT "\t{0x%06x, 0x%06x},\n", $range->{start}, $range->{end};
}

print $OT "};\n";
