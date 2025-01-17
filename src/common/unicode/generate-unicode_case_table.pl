#!/usr/bin/perl
#
# Generate Unicode character case mappings. Does not include tailoring
# or locale-specific mappings.
#
# Input: SpecialCasing.txt UnicodeData.txt
# Output: unicode_case_table.h
#
# Copyright (c) 2000-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use Getopt::Long;

use FindBin;
use lib "$FindBin::RealBin/../../tools/";

my $output_path = '.';

GetOptions('outdir:s' => \$output_path);

my $output_table_file = "$output_path/unicode_case_table.h";

# The maximum number of codepoints that can result from case mapping
# of a single character. See Unicode section 5.18 "Case Mappings".
my $MAX_CASE_EXPANSION = 3;

my $FH;

my %simple = ();

open($FH, '<', "$output_path/UnicodeData.txt")
  or die "Could not open $output_path/UnicodeData.txt: $!.";
while (my $line = <$FH>)
{
	my @elts = split(';', $line);
	my $code = hex($elts[0]);
	my $simple_uppercase = hex($elts[12] =~ s/^\s+|\s+$//rg);
	my $simple_lowercase = hex($elts[13] =~ s/^\s+|\s+$//rg);
	my $simple_titlecase = hex($elts[14] =~ s/^\s+|\s+$//rg);

	die "codepoint $code out of range" if $code > 0x10FFFF;
	die "Simple_Lowercase $code out of range" if $simple_lowercase > 0x10FFFF;
	die "Simple_Titlecase $code out of range" if $simple_titlecase > 0x10FFFF;
	die "Simple_Uppercase $code out of range" if $simple_uppercase > 0x10FFFF;

	if ($simple_lowercase || $simple_titlecase || $simple_uppercase)
	{
		$simple{$code} = {
			Simple_Lowercase => ($simple_lowercase || $code),
			Simple_Titlecase => ($simple_titlecase || $code),
			Simple_Uppercase => ($simple_uppercase || $code)
		};
	}
}
close $FH;

# Map for special casing rules that aren't represented in the simple
# mapping. Language-sensitive mappings are not supported.
#
# See https://www.unicode.org/reports/tr44/#SpecialCasing.txt, or the
# SpecialCasing.txt file itself for details.

# for now, only Final_Sigma is supported
my %condition_map = (Final_Sigma => 'PG_U_FINAL_SIGMA');

my %special = ();
open($FH, '<', "$output_path/SpecialCasing.txt")
  or die "Could not open $output_path/SpecialCasing.txt: $!.";
while (my $line = <$FH>)
{
	# language-sensitive mappings not supported
	last if $line =~ /\# Language-Sensitive Mappings/;

	# remove comments
	$line =~ s/^(.*?)#.*$/$1/s;

	# ignore empty lines
	next unless $line =~ /;/;

	my @elts = split /;/, $line;
	my $code = hex($elts[0]);

	# Codepoint may map to multiple characters when converting
	# case. Split each mapping on whitespace and extract the
	# hexadecimal into an array of codepoints.
	my @lower = map { hex $_ } (grep /^[0-9A-F]+$/, (split /\s+/, $elts[1]));
	my @title = map { hex $_ } (grep /^[0-9A-F]+$/, (split /\s+/, $elts[2]));
	my @upper = map { hex $_ } (grep /^[0-9A-F]+$/, (split /\s+/, $elts[3]));
	my @conditions = map {
		# supporting negated conditions may require storing a
		# mask of relevant conditions for a given rule to differentiate
		# between lack of a condition and a negated condition
		die "negated conditions not supported" if /^Not_/;
		$condition_map{$_} || die "unrecognized condition: $_"
	} (grep /\w+/, (split /\s+/, $elts[4]));

	my $cond_str = (join '|', @conditions) || '0';

	# if empty, create a self-mapping
	push @lower, $code if (scalar @lower == 0);
	push @title, $code if (scalar @title == 0);
	push @upper, $code if (scalar @upper == 0);

	# none should map to more than 3 codepoints
	die "lowercase expansion for 0x$elts[0] exceeds maximum: '$elts[1]'"
	  if (scalar @lower) > $MAX_CASE_EXPANSION;
	die "titlecase expansion for 0x$elts[0] exceeds maximum: '$elts[2]'"
	  if (scalar @title) > $MAX_CASE_EXPANSION;
	die "uppercase expansion for 0x$elts[0] exceeds maximum: '$elts[3]'"
	  if (scalar @upper) > $MAX_CASE_EXPANSION;

	# pad arrays to a fixed length of 3
	while (scalar @upper < $MAX_CASE_EXPANSION) { push @upper, 0x000000 }
	while (scalar @lower < $MAX_CASE_EXPANSION) { push @lower, 0x000000 }
	while (scalar @title < $MAX_CASE_EXPANSION) { push @title, 0x000000 }

	# Characters with special mappings may not have simple mappings;
	# ensure that an entry exists.
	$simple{$code} ||= {
		Simple_Lowercase => $code,
		Simple_Titlecase => $code,
		Simple_Uppercase => $code
	};

	# Multiple special case rules for a single codepoint could be
	# supported by making several entries for each codepoint, and have
	# the simple mapping point to the first entry. The caller could
	# scan forward looking for an entry that matches the conditions,
	# or fall back to the normal behavior.
	die "multiple special case mappings not supported"
	  if defined $special{$code};

	$special{$code} = {
		Lowercase => \@lower,
		Titlecase => \@title,
		Uppercase => \@upper,
		Conditions => $cond_str
	};
}
close $FH;

# assign sequential array indexes to the special mappings
my $special_idx = 0;
foreach my $code (sort { $a <=> $b } (keys %special))
{
	$special{$code}{Index} = $special_idx++;
}

# Start writing out the output files
open my $OT, '>', $output_table_file
  or die "Could not open output file $output_table_file: $!\n";

# determine size of array given that codepoints <= 0x80 are dense and
# the rest of the entries are sparse
my $num_simple = 0x80;
foreach my $code (sort { $a <=> $b } (keys %simple))
{
	$num_simple++ unless $code < 0x80;
}

my $num_special = scalar(keys %special) + 1;

print $OT <<"EOS";
/*-------------------------------------------------------------------------
 *
 * unicode_case_table.h
 *	  Case mapping and information table.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/unicode_case_table.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * File auto-generated by src/common/unicode/generate-unicode_case_table.pl,
 * do not edit. There is deliberately not an #ifndef PG_UNICODE_CASE_TABLE_H
 * here.
 */

#include "common/unicode_case.h"
#include "mb/pg_wchar.h"

/*
 * The maximum number of codepoints that can result from case mapping
 * of a single character. See Unicode section 5.18 "Case Mappings".
 */
#define MAX_CASE_EXPANSION 3

/*
 * Case mapping condition flags. For now, only Final_Sigma is supported.
 *
 * See Unicode Context Specification for Casing.
 */
#define PG_U_FINAL_SIGMA		(1 << 0)

typedef enum
{
	CaseLower = 0,
	CaseTitle = 1,
	CaseUpper = 2,
	NCaseKind
} CaseKind;

typedef struct
{
	pg_wchar	codepoint;		/* Unicode codepoint */
	int16		conditions;
	pg_wchar	map[NCaseKind][MAX_CASE_EXPANSION];
} pg_special_case;

typedef struct
{
	pg_wchar	codepoint;		/* Unicode codepoint */
	pg_wchar	simplemap[NCaseKind];
	const pg_special_case *special_case;
} pg_case_map;

/*
 * Special case mappings that aren't representable in the simple map.
 * Entries are referenced from simple_case_map.
 */
static const pg_special_case special_case[$num_special] =
{
EOS

foreach my $code (sort { $a <=> $b } (keys %special))
{
	die if scalar @{ $special{$code}{Lowercase} } != $MAX_CASE_EXPANSION;
	die if scalar @{ $special{$code}{Titlecase} } != $MAX_CASE_EXPANSION;
	die if scalar @{ $special{$code}{Uppercase} } != $MAX_CASE_EXPANSION;
	my $lower = join ", ",
	  (map { sprintf "0x%06x", $_ } @{ $special{$code}{Lowercase} });
	my $title = join ", ",
	  (map { sprintf "0x%06x", $_ } @{ $special{$code}{Titlecase} });
	my $upper = join ", ",
	  (map { sprintf "0x%06x", $_ } @{ $special{$code}{Uppercase} });
	printf $OT "\t{0x%06x, %s, ", $code, $special{$code}{Conditions};
	printf $OT "{{%s}, {%s}, {%s}}},\n", $lower, $title, $upper;
}

print $OT "\t{0, 0, {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}}\n";
print $OT <<"EOS";
};

/*
 * Case mapping table. Dense for codepoints < 0x80 (enabling fast lookup),
 * sparse for higher codepoints (requiring scan or binary search).
 */
static const pg_case_map case_map[$num_simple] =
{
EOS

printf $OT "\t/* begin dense entries for codepoints < 0x80 */\n";
for (my $code = 0; $code < 0x80; $code++)
{
	my $lc = ($simple{$code}{Simple_Lowercase} || $code);
	my $tc = ($simple{$code}{Simple_Titlecase} || $code);
	my $uc = ($simple{$code}{Simple_Uppercase} || $code);
	die "unexpected special case for code $code"
	  if defined $special{$code};
	printf $OT
	  "\t{0x%06x, {[CaseLower] = 0x%06x,[CaseTitle] = 0x%06x,[CaseUpper] = 0x%06x}, NULL},\n",
	  $code, $lc, $tc, $uc;
}
printf $OT "\n";

printf $OT "\t/* begin sparse entries for codepoints >= 0x80 */\n";
foreach my $code (sort { $a <=> $b } (keys %simple))
{
	next unless $code >= 0x80;    # already output above

	my $map = $simple{$code};
	my $special_case = "NULL";
	if (exists $special{$code})
	{
		$special_case = sprintf "&special_case[%d]", $special{$code}{Index};
	}
	printf $OT
	  "\t{0x%06x, {[CaseLower] = 0x%06x,[CaseTitle] = 0x%06x,[CaseUpper] = 0x%06x}, %s},\n",
	  $code, $map->{Simple_Lowercase}, $map->{Simple_Titlecase},
	  $map->{Simple_Uppercase}, $special_case;
}
print $OT "};\n";
