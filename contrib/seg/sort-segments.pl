#!/usr/bin/perl

# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# this script will sort any table with the segment data type in its last column

use strict;
use warnings FATAL => 'all';

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
