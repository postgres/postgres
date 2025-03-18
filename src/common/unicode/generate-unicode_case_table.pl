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
			Simple_Uppercase => ($simple_uppercase || $code),
			Simple_Foldcase => $code,
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
	my @fold = ();
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
	push @fold, $code;

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
	while (scalar @fold < $MAX_CASE_EXPANSION)  { push @fold, 0x000000 }

	# Characters with special mappings may not have simple mappings;
	# ensure that an entry exists.
	$simple{$code} ||= {
		Simple_Lowercase => $code,
		Simple_Titlecase => $code,
		Simple_Uppercase => $code,
		Simple_Foldcase => $code
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
		Foldcase => \@fold,
		Conditions => $cond_str
	};
}
close $FH;

open($FH, '<', "$output_path/CaseFolding.txt")
  or die "Could not open $output_path/CaseFolding.txt: $!.";
while (my $line = <$FH>)
{
	# remove comments
	$line =~ s/^(.*?)#.*$/$1/s;

	# ignore empty lines
	next unless $line =~ /;/;

	my @elts = split(';', $line);
	my $code = hex($elts[0]);
	my $status = $elts[1] =~ s/^\s+|\s+$//rg;

	# Codepoint may map to multiple characters when folding. Split
	# each mapping on whitespace and extract the hexadecimal into an
	# array of codepoints.
	my @fold = map { hex $_ } (grep /[0-9A-F]+/, (split /\s+/, $elts[2]));

	die "codepoint $code out of range" if $code > 0x10FFFF;

	# status 'T' unsupported; skip
	next if $status eq 'T';

	# encountered unrecognized status type
	die "unsupported status type '$status'"
	  if $status ne 'S' && $status ne 'C' && $status ne 'F';

	# initialize simple case mappings if they don't exist
	$simple{$code} ||= {
		Simple_Lowercase => $code,
		Simple_Titlecase => $code,
		Simple_Uppercase => $code,
		Simple_Foldcase => $code
	};

	if ($status eq 'S' || $status eq 'C')
	{
		die
		  "Simple case folding for $code has multiple codepoints: '$line' '$elts[2]'"
		  if scalar @fold != 1;
		my $simple_foldcase = $fold[0];

		die "Simple_Foldcase $code out of range"
		  if $simple_foldcase > 0x10FFFF;

		$simple{$code}{Simple_Foldcase} = $simple_foldcase;
	}

	if ($status eq 'F' || ($status eq 'C' && defined $special{$code}))
	{
		while (scalar @fold < $MAX_CASE_EXPANSION) { push @fold, 0x000000 }

		#initialize special case mappings if they don't exist
		if (!defined $special{$code})
		{
			my @lower = ($simple{$code}{Simple_Lowercase});
			my @title = ($simple{$code}{Simple_Titlecase});
			my @upper = ($simple{$code}{Simple_Uppercase});
			while (scalar @lower < $MAX_CASE_EXPANSION)
			{
				push @lower, 0x000000;
			}
			while (scalar @title < $MAX_CASE_EXPANSION)
			{
				push @title, 0x000000;
			}
			while (scalar @upper < $MAX_CASE_EXPANSION)
			{
				push @upper, 0x000000;
			}
			$special{$code} = {
				Lowercase => \@lower,
				Titlecase => \@title,
				Uppercase => \@upper,
				Conditions => '0'
			};
		}

		$special{$code}{Foldcase} = \@fold;
	}
}
close $FH;

# assign sequential array indexes to the special mappings
# 0 is reserved for NULL
my $special_idx = 1;
foreach my $code (sort { $a <=> $b } (keys %special))
{
	$special{$code}{Index} = $special_idx++;
}

# determine size of array
my $num_special = scalar(keys %special) + 1;

die
  "special case map contains $num_special entries which cannot be represented in uint8"
  if ($num_special > 256);

# Start writing out the output files
open my $OT, '>', $output_table_file
  or die "Could not open output file $output_table_file: $!\n";

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
	CaseFold = 3,
	NCaseKind
} CaseKind;

typedef struct
{
	int16		conditions;
	pg_wchar	map[NCaseKind][MAX_CASE_EXPANSION];
} pg_special_case;

/*
 * Special case mappings that aren't representable in the simple map.
 * Entries are referenced from simple_case_map.
 */
static const pg_special_case special_case[$num_special] =
{
	{0, {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}},
EOS

foreach my $code (sort { $a <=> $b } (keys %special))
{
	die if scalar @{ $special{$code}{Lowercase} } != $MAX_CASE_EXPANSION;
	die if scalar @{ $special{$code}{Titlecase} } != $MAX_CASE_EXPANSION;
	die if scalar @{ $special{$code}{Uppercase} } != $MAX_CASE_EXPANSION;
	die if scalar @{ $special{$code}{Foldcase} } != $MAX_CASE_EXPANSION;
	my $lower = join ", ",
	  (map { sprintf "0x%06x", $_ } @{ $special{$code}{Lowercase} });
	my $title = join ", ",
	  (map { sprintf "0x%06x", $_ } @{ $special{$code}{Titlecase} });
	my $upper = join ", ",
	  (map { sprintf "0x%06x", $_ } @{ $special{$code}{Uppercase} });
	my $fold = join ", ",
	  (map { sprintf "0x%06x", $_ } @{ $special{$code}{Foldcase} });
	printf $OT "\t{%s, ", $special{$code}{Conditions};
	printf $OT
	  "{[CaseLower] = {%s},[CaseTitle] = {%s},[CaseUpper] = {%s},[CaseFold] = {%s}}},\n",
	  $lower, $title, $upper, $fold;
}
print $OT "};\n";

# Separate maps for each case form, starting with the reserved entry
# at index 0. The first element is the result code point, and the
# second element is the input code point (which is not ultimately
# stored in the C array, it's just there as a comment).
my %map = (
	lower => [ [ 0, -1 ] ],
	title => [ [ 0, -1 ] ],
	upper => [ [ 0, -1 ] ],
	fold => [ [ 0, -1 ] ],
	special => [ [ 0, -1 ] ]);


# Current index into the map arrays above.
my $index = 1;

# Sets of case forms/variations. Simple case pairs have the same set
# of case forms, e.g. the letters 'a' and 'A' both lowercase to 'a';
# both uppercase to 'A', etc. By tracking unique sets using a hash, we
# cut the size needed for the maps in half (some characters are
# exceptions, so it's not exactly half). The key is an array of all
# case forms, and the value is an index into the maps.
my %case_forms;

# Perl doesn't allow arrays as hash keys, so we need to transform the
# set of case forms to a scalar.
sub get_hash_key
{
	return join ",", @_;
}

# Create map entries for all codepoints < 0x80, so that the caller can
# have a fast-path lookup without needing to go through the main
# table.
for (my $code = 0; $code < 0x80; $code++)
{
	my $lc = ($simple{$code}{Simple_Lowercase} || $code);
	my $tc = ($simple{$code}{Simple_Titlecase} || $code);
	my $uc = ($simple{$code}{Simple_Uppercase} || $code);
	my $fc = ($simple{$code}{Simple_Foldcase} || $code);

	die "unexpected special case for code $code"
	  if defined $special{$code};

	push @{ $map{lower} }, [ $lc, $code ];
	push @{ $map{title} }, [ $tc, $code ];
	push @{ $map{upper} }, [ $uc, $code ];
	push @{ $map{fold} }, [ $fc, $code ];
	push @{ $map{special} }, [ 0, $code ];

	my $key = get_hash_key($lc, $tc, $uc, $fc, 0);

	$simple{$code}{Index} = $index;
	$case_forms{$key} = $index++;
}

# Create map entries for all characters >= 0x80 that have case
# mappings (any character with a special case mapping also has an
# entry in %simple).
foreach my $code (sort { $a <=> $b } (keys %simple))
{
	next unless $code >= 0x80;    # already output above

	my $entry = $simple{$code};
	my $special_case = 0;
	if (exists $special{$code})
	{
		$special_case = $special{$code}{Index};
	}

	my $key = get_hash_key(
		$entry->{Simple_Lowercase}, $entry->{Simple_Titlecase},
		$entry->{Simple_Uppercase}, $entry->{Simple_Foldcase},
		$special_case);

	unless (exists $case_forms{$key})
	{
		$case_forms{$key} = $index++;

		push @{ $map{lower} }, [ $entry->{Simple_Lowercase}, $code ];
		push @{ $map{title} }, [ $entry->{Simple_Titlecase}, $code ];
		push @{ $map{upper} }, [ $entry->{Simple_Uppercase}, $code ];
		push @{ $map{fold} }, [ $entry->{Simple_Foldcase}, $code ];
		push @{ $map{special} }, [ $special_case, $code ];
	}

	$simple{$code}{Index} = $case_forms{$key};
}

die
  "mapping tables contains $index entries which cannot be represented in uint16"
  if ($index > 65536);

foreach my $kind ('lower', 'title', 'upper', 'fold')
{
	print $OT <<"EOS";

/*
 * The entry case_map_${kind}[case_index(codepoint)] is the mapping for the
 * given codepoint.
 */
static const pg_wchar case_map_$kind\[$index\] =
{
EOS

	foreach my $entry (@{ $map{$kind} })
	{
		my $comment =
		  @$entry[1] == -1 ? "reserved" : sprintf("U+%06x", @$entry[1]);
		print $OT
		  sprintf("\t0x%06x,\t\t\t\t\t/* %s */\n", @$entry[0], $comment);
	}

	print $OT "\n};\n";
}

print $OT <<"EOS";

/*
 * The entry case_map_special[case_index(codepoint)] is the index in
 * special_case for that codepoint, or 0 if no special case mapping exists.
 */
static const uint8 case_map_special\[$index\] =
{
EOS

foreach my $entry (@{ $map{special} })
{
	my $s = sprintf("%d,", @$entry[0]);
	$s .= "\t" if length($s) < 4;
	my $comment =
	  @$entry[1] == -1 ? "reserved" : sprintf("U+%06x", @$entry[1]);
	print $OT sprintf("\t%s\t\t\t\t\t\t/* %s */\n", $s, $comment);
}

print $OT "\n};\n";

my @codepoints = keys %simple;
my $range = make_ranges(\@codepoints, 500);
my @case_map_lines = range_tables($range);
my $case_map_length = scalar @case_map_lines;
my $case_map_table = join "\n", @case_map_lines;

print $OT <<"EOS";

/*
 * Used by case_index() to map a codepoint to an index that can be used in any
 * of the following arrays: case_map_lower, case_map_title, case_map_upper,
 * case_map_fold.
 */
static const uint16 case_map[$case_map_length] =
{
$case_map_table
};


EOS

# First range is the fast path. It must start at codepoint zero, and
# the end is the fastpath limit. Track the limit here and then
# remove it before generating the other branches.
die "first range must start at 0" unless ${ @$range[0] }{Start} == 0;
my $fastpath_limit = sprintf("0x%04X", ${ @$range[0] }{End});
shift @$range;

print $OT <<"EOS";
/*
 * case_index()
 *
 * Given a code point, compute the index in the case_map at which we can find
 * the offset into the mapping tables.
 */
static inline uint16
case_index(pg_wchar cp)
{
	/* Fast path for codepoints < $fastpath_limit */
	if (cp < $fastpath_limit)
	{
		return case_map[cp];
	}

EOS

print $OT join("\n", @{ branch($range, 0, $#$range, 1) });

print $OT <<"EOS";


	return 0;
}
EOS

close $OT;

# The function generates C code with a series of nested if-else conditions
# to search for the matching interval.
sub branch
{
	my ($range, $from, $to, $indent) = @_;
	my ($idx, $space, $entry, $table, @result);

	$idx = ($from + int(($to - $from) / 2));
	return \@result unless exists $range->[$idx];

	$space = "\t" x $indent;

	$entry = $range->[$idx];

	# IF state
	if ($idx == $from)
	{
		if ($idx == 0)
		{
			push @result,
			  sprintf("%sif (cp >= 0x%04X && cp < 0x%04X)\n%s{",
				$space, $entry->{Start}, $entry->{End}, $space);
		}
		else
		{
			push @result,
			  sprintf("%sif (cp < 0x%04X)\n%s{",
				$space, $entry->{End}, $space);
		}

		push @result,
		  sprintf("%s\treturn case_map[cp - 0x%04X + %d];",
			$space, $entry->{Start}, $entry->{Offset});
	}
	else
	{
		push @result,
		  sprintf("%sif (cp < 0x%04X)\n%s{", $space, $entry->{End}, $space);
		push @result, @{ branch($range, $from, $idx - 1, $indent + 1) };
	}

	push @result, $space . "}";

	# return now if it's the last range
	return \@result if $idx == (scalar @$range) - 1;

	# ELSE looks ahead to the next range to avoid adding an
	# unnecessary level of branching.
	$entry = @$range[ $idx + 1 ];

	# ELSE state
	push @result,
	  sprintf("%selse if (cp >= 0x%04X)\n%s{",
		$space, $entry->{Start}, $space);

	if ($idx == $to)
	{
		push @result,
		  sprintf("%s\treturn case_map\[cp - 0x%04X + %d];",
			$space, $entry->{Start}, $entry->{Offset});
	}
	else
	{
		push @result, @{ branch($range, $idx + 1, $to, $indent + 1) };
	}

	push @result, $space . "}";

	return \@result;
}

# Group numbers into ranges where the difference between neighboring
# elements does not exceed $limit. If the difference is greater, a new
# range is created. This is used to break the sequence into intervals
# where the gaps between numbers are greater than limit.
#
# For example, if there are numbers 1, 2, 3, 5, 6 and limit = 1, then
# there is a difference of 2 between 3 and 5, which is greater than 1,
# so there will be ranges 1-3 and 5-6.
sub make_ranges
{
	my ($nums, $limit) = @_;
	my ($prev, $start, $total, @sorted, @range);

	@sorted = sort { $a <=> $b } @$nums;

	die "expecting at least 2 codepoints" if (scalar @sorted < 2);

	$start = shift @sorted;

	die "expecting first codepoint to start at 0" unless $start == 0;

	$prev = $start;
	$total = 0;

	# append final 'undef' to signal final iteration
	push @sorted, undef;

	foreach my $curr (@sorted)
	{
		# if last iteration always append the range
		if (!defined($curr) || ($curr - $prev > $limit))
		{
			push @range,
			  {
				Start => $start,
				End => $prev + 1,
				Offset => $total
			  };
			$total += $prev + 1 - $start;
			$start = $curr;
		}

		$prev = $curr;
	}

	return \@range;
}

# The function combines all ranges into the case_map table. Ranges may
# include codepoints without a case mapping at all, in which case the
# entry in case_map should be zero.
sub range_tables
{
	my ($range) = @_;
	my (@lines, @result);

	foreach my $entry (@$range)
	{
		my $start = $entry->{Start};
		my $end = $entry->{End} - 1;

		foreach my $cp ($start .. $end)
		{
			my $idx = sprintf("%d,", ($simple{$cp}{Index} || 0));
			$idx .= "\t" if length($idx) < 4;
			push @lines, sprintf("\t%s\t\t\t\t\t\t/* U+%06X */", $idx, $cp);
		}
	}

	return @lines;
}
