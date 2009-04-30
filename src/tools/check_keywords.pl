#!/usr/bin/perl -w

use strict;

# Check that the keyword lists in gram.y and kwlist.h are sane. Run from
# the top directory, or pass a path to a top directory as argument.
#
# $PostgreSQL: pgsql/src/tools/check_keywords.pl,v 1.2 2009/04/30 10:26:35 heikki Exp $

my $path;

if (@ARGV) {
	$path = $ARGV[0];
	shift @ARGV;
} else {
	$path = "."; 
}

$[ = 1;			# set array base to 1
$, = ' ';		# set output field separator
$\ = "\n";		# set output record separator

my %keyword_categories;
$keyword_categories{'unreserved_keyword'} = 'UNRESERVED_KEYWORD';
$keyword_categories{'col_name_keyword'} = 'COL_NAME_KEYWORD';
$keyword_categories{'type_func_name_keyword'} = 'TYPE_FUNC_NAME_KEYWORD';
$keyword_categories{'reserved_keyword'} = 'RESERVED_KEYWORD';

my $gram_filename = "$path/src/backend/parser/gram.y";
open(GRAM, $gram_filename) || die("Could not open : $gram_filename");

my ($S, $s, $k, $n, $kcat);
my $comment;
my @arr;
my %keywords;

line: while (<GRAM>) {
    chomp;	# strip record separator

    $S = $_;
    # Make sure any braces are split
    $s = '{', $S =~ s/$s/ { /g;
    $s = '}', $S =~ s/$s/ } /g;
    # Any comments are split
    $s = '[/][*]', $S =~ s#$s# /* #g;
    $s = '[*][/]', $S =~ s#$s# */ #g;

    if (!($kcat)) {
	# Is this the beginning of a keyword list?
	foreach $k (keys %keyword_categories) {
	    if ($S =~ m/^($k):/) {
		$kcat = $k;
		next line;
	    }
	}
	next line;
    }

    # Now split the line into individual fields
    $n = (@arr = split(' ', $S));

    # Ok, we're in a keyword list. Go through each field in turn
    for (my $fieldIndexer = 1; $fieldIndexer <= $n; $fieldIndexer++) {
	if ($arr[$fieldIndexer] eq '*/' && $comment) {
	    $comment = 0;
	    next;
	}
	elsif ($comment) {
	    next;
	}
	elsif ($arr[$fieldIndexer] eq '/*') {
	    # start of a multiline comment
	    $comment = 1;
	    next;
	}
	elsif ($arr[$fieldIndexer] eq '//') {
	    next line;
	}

	if ($arr[$fieldIndexer] eq ';') {
	    # end of keyword list
	    $kcat = '';
	    next;
	}

	if ($arr[$fieldIndexer] eq '|') {
	    next;
	}
	
	# Put this keyword into the right list
	push @{$keywords{$kcat}}, $arr[$fieldIndexer];
    }
}
close GRAM;

# Check that all keywords are in alphabetical order
my ($prevkword, $kword, $bare_kword);
foreach $kcat (keys %keyword_categories) {
    $prevkword = '';

    foreach $kword (@{$keywords{$kcat}}) {
	# Some keyword have a _P suffix. Remove it for the comparison.
	$bare_kword = $kword;
	$bare_kword =~ s/_P$//;
	if ($bare_kword le $prevkword) {
	    print "'$bare_kword' after '$prevkword' in $kcat list is misplaced";
	}
	$prevkword = $bare_kword;
    }
}

# Transform the keyword lists into hashes.
# kwhashes is a hash of hashes, keyed by keyword category id, e.g.
# UNRESERVED_KEYWORD. Each inner hash is a keyed by keyword id, e.g. ABORT_P
# with a dummy value.
my %kwhashes;
while ( my ($kcat, $kcat_id) = each(%keyword_categories) ) {
    @arr = @{$keywords{$kcat}};

    my $hash;
    foreach my $item (@arr) { $hash->{$item} = 1 }

    $kwhashes{$kcat_id} = $hash;
}

# Now read in kwlist.h

my $kwlist_filename = "$path/src/include/parser/kwlist.h";
open(KWLIST, $kwlist_filename) || die("Could not open : $kwlist_filename");

my $prevkwstring = '';
my $bare_kwname;
my %kwhash;
kwlist_line: while (<KWLIST>) {
    my($line) = $_;

    if ($line =~ /^PG_KEYWORD\(\"(.*)\", (.*), (.*)\)/)
    {
	my($kwstring) = $1;
	my($kwname) = $2;
	my($kwcat_id) = $3;

	# Check that the list is in alphabetical order
	if ($kwstring le $prevkwstring) {
	    print "'$kwstring' after '$prevkwstring' in kwlist.h is misplaced";
	}
	$prevkwstring = $kwstring;

	# Check that the keyword string is valid: all lower-case ASCII chars
	if ($kwstring !~ /^[a-z_]*$/) {
	    print "'$kwstring' is not a valid keyword string, must be all lower-case ASCII chars";
	}

	# Check that the keyword name is valid: all upper-case ASCII chars
	if ($kwname !~ /^[A-Z_]*$/) {
	    print "'$kwname' is not a valid keyword name, must be all upper-case ASCII chars";
	}

	# Check that the keyword string matches keyword name
	$bare_kwname = $kwname;
	$bare_kwname =~ s/_P$//;
	if ($bare_kwname ne uc($kwstring)) {
	    print "keyword name '$kwname' doesn't match keyword string '$kwstring'";
	}

	# Check that the keyword is present in the grammar
	%kwhash = %{$kwhashes{$kwcat_id}};

	if (!(%kwhash))	{
	    #print "Unknown kwcat_id: $kwcat_id";
	} else {
	    if (!($kwhash{$kwname})) {
		print "'$kwname' not present in $kwcat_id section of gram.y";
	    } else {
		# Remove it from the hash, so that we can complain at the end
		# if there's keywords left that were not found in kwlist.h
		delete $kwhashes{$kwcat_id}->{$kwname};
	    }
	}
    }
}
close KWLIST;

# Check that we've paired up all keywords from gram.y with lines in kwlist.h
while ( my ($kwcat, $kwcat_id) = each(%keyword_categories) ) {
    %kwhash = %{$kwhashes{$kwcat_id}};

    for my $kw ( keys %kwhash ) {
	print "'$kw' found in gram.y $kwcat category, but not in kwlist.h"
    }
}
