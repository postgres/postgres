#!/usr/bin/perl

# this script will sort any table with the segment data type in its last column

use strict;
use warnings;

my @rows;

while (<>)
{
	chomp;
	push @rows, $_;
}

foreach (
	sort {
		my @ar = split("\t", $a);
		my $valA = pop @ar;
		$valA =~ s/[~<> ]+//g;
		@ar = split("\t", $b);
		my $valB = pop @ar;
		$valB =~ s/[~<> ]+//g;
		$valA <=> $valB
	} @rows)
{
	print "$_\n";
}
