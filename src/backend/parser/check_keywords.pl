#!/usr/bin/perl

# Check that the keyword lists in gram.y and kwlist.h are sane.
# Usage: check_keywords.pl gram.y kwlist.h

# src/backend/parser/check_keywords.pl
# Copyright (c) 2009-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

my $gram_filename = $ARGV[0];
my $kwlist_filename = $ARGV[1];

my $errors = 0;

sub error
{
	print STDERR @_;
	$errors = 1;
	return;
}

# Check alphabetical order of a set of keyword symbols
# (note these are NOT the actual keyword strings)
sub check_alphabetical_order
{
	my ($listname, $list) = @_;
	my $prevkword = '';

	foreach my $kword (@$list)
	{
		# Some symbols have a _P suffix. Remove it for the comparison.
		my $bare_kword = $kword;
		$bare_kword =~ s/_P$//;
		if ($bare_kword le $prevkword)
		{
			error
			  "'$bare_kword' after '$prevkword' in $listname list is misplaced";
		}
		$prevkword = $bare_kword;
	}
	return;
}

$, = ' ';     # set output field separator
$\ = "\n";    # set output record separator

my %keyword_categories;
$keyword_categories{'unreserved_keyword'} = 'UNRESERVED_KEYWORD';
$keyword_categories{'col_name_keyword'} = 'COL_NAME_KEYWORD';
$keyword_categories{'type_func_name_keyword'} = 'TYPE_FUNC_NAME_KEYWORD';
$keyword_categories{'reserved_keyword'} = 'RESERVED_KEYWORD';

open(my $gram, '<', $gram_filename) || die("Could not open : $gram_filename");

my $kcat;
my $in_bare_labels;
my $comment;
my @arr;
my %keywords;
my @bare_label_keywords;

line: while (my $S = <$gram>)
{
	chomp $S;    # strip record separator

	my $s;

	# Make sure any braces are split
	$s = '{', $S =~ s/$s/ { /g;
	$s = '}', $S =~ s/$s/ } /g;

	# Any comments are split
	$s = '[/][*]', $S =~ s#$s# /* #g;
	$s = '[*][/]', $S =~ s#$s# */ #g;

	if (!($kcat) && !($in_bare_labels))
	{

		# Is this the beginning of a keyword list?
		foreach my $k (keys %keyword_categories)
		{
			if ($S =~ m/^($k):/)
			{
				$kcat = $k;
				next line;
			}
		}

		# Is this the beginning of the bare_label_keyword list?
		$in_bare_labels = 1 if ($S =~ m/^bare_label_keyword:/);

		next line;
	}

	# Now split the line into individual fields
	my $n = (@arr = split(' ', $S));

	# Ok, we're in a keyword list. Go through each field in turn
	for (my $fieldIndexer = 0; $fieldIndexer < $n; $fieldIndexer++)
	{
		if ($arr[$fieldIndexer] eq '*/' && $comment)
		{
			$comment = 0;
			next;
		}
		elsif ($comment)
		{
			next;
		}
		elsif ($arr[$fieldIndexer] eq '/*')
		{

			# start of a multiline comment
			$comment = 1;
			next;
		}
		elsif ($arr[$fieldIndexer] eq '//')
		{
			next line;
		}

		if ($arr[$fieldIndexer] eq ';')
		{

			# end of keyword list
			undef $kcat;
			undef $in_bare_labels;
			next;
		}

		if ($arr[$fieldIndexer] eq '|')
		{
			next;
		}

		# Put this keyword into the right list
		if ($in_bare_labels)
		{
			push @bare_label_keywords, $arr[$fieldIndexer];
		}
		else
		{
			push @{ $keywords{$kcat} }, $arr[$fieldIndexer];
		}
	}
}
close $gram;

# Check that each keyword list is in alphabetical order (just for neatnik-ism)
check_alphabetical_order($_, $keywords{$_}) for (keys %keyword_categories);
check_alphabetical_order('bare_label_keyword', \@bare_label_keywords);

# Transform the keyword lists into hashes.
# kwhashes is a hash of hashes, keyed by keyword category id,
# e.g. UNRESERVED_KEYWORD.
# Each inner hash is keyed by keyword id, e.g. ABORT_P, with a dummy value.
my %kwhashes;
while (my ($kcat, $kcat_id) = each(%keyword_categories))
{
	@arr = @{ $keywords{$kcat} };

	my $hash;
	foreach my $item (@arr) { $hash->{$item} = 1; }

	$kwhashes{$kcat_id} = $hash;
}
my %bare_label_keywords = map { $_ => 1 } @bare_label_keywords;

# Now read in kwlist.h

open(my $kwlist, '<', $kwlist_filename)
  || die("Could not open : $kwlist_filename");

my $prevkwstring = '';
my $bare_kwname;
my %kwhash;
kwlist_line: while (<$kwlist>)
{
	my ($line) = $_;

	if ($line =~ /^PG_KEYWORD\(\"(.*)\", (.*), (.*), (.*)\)/)
	{
		my ($kwstring) = $1;
		my ($kwname) = $2;
		my ($kwcat_id) = $3;
		my ($collabel) = $4;

		# Check that the list is in alphabetical order (critical!)
		if ($kwstring le $prevkwstring)
		{
			error
			  "'$kwstring' after '$prevkwstring' in kwlist.h is misplaced";
		}
		$prevkwstring = $kwstring;

		# Check that the keyword string is valid: all lower-case ASCII chars
		if ($kwstring !~ /^[a-z_]+$/)
		{
			error
			  "'$kwstring' is not a valid keyword string, must be all lower-case ASCII chars";
		}

		# Check that the keyword name is valid: all upper-case ASCII chars
		if ($kwname !~ /^[A-Z_]+$/)
		{
			error
			  "'$kwname' is not a valid keyword name, must be all upper-case ASCII chars";
		}

		# Check that the keyword string matches keyword name
		$bare_kwname = $kwname;
		$bare_kwname =~ s/_P$//;
		if ($bare_kwname ne uc($kwstring))
		{
			error
			  "keyword name '$kwname' doesn't match keyword string '$kwstring'";
		}

		# Check that the keyword is present in the right category list
		%kwhash = %{ $kwhashes{$kwcat_id} };

		if (!(%kwhash))
		{
			error "Unknown keyword category: $kwcat_id";
		}
		else
		{
			if (!($kwhash{$kwname}))
			{
				error "'$kwname' not present in $kwcat_id section of gram.y";
			}
			else
			{

				# Remove it from the hash, so that we can
				# complain at the end if there's keywords left
				# that were not found in kwlist.h
				delete $kwhashes{$kwcat_id}->{$kwname};
			}
		}

		# Check that the keyword's collabel property matches gram.y
		if ($collabel eq 'BARE_LABEL')
		{
			unless ($bare_label_keywords{$kwname})
			{
				error
				  "'$kwname' is marked as BARE_LABEL in kwlist.h, but it is missing from gram.y's bare_label_keyword rule";
			}
		}
		elsif ($collabel eq 'AS_LABEL')
		{
			if ($bare_label_keywords{$kwname})
			{
				error
				  "'$kwname' is marked as AS_LABEL in kwlist.h, but it is listed in gram.y's bare_label_keyword rule";
			}
		}
		else
		{
			error
			  "'$collabel' not recognized in kwlist.h.  Expected either 'BARE_LABEL' or 'AS_LABEL'";
		}
	}
}
close $kwlist;

# Check that we've paired up all keywords from gram.y with lines in kwlist.h
while (my ($kwcat, $kwcat_id) = each(%keyword_categories))
{
	%kwhash = %{ $kwhashes{$kwcat_id} };

	for my $kw (keys %kwhash)
	{
		error "'$kw' found in gram.y $kwcat category, but not in kwlist.h";
	}
}

exit $errors;
