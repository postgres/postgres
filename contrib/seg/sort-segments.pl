#!/usr/bin/perl

# this script will sort any table with the segment data type in its last column

while (<>)
{
	chomp;
	push @rows, $_;
}

foreach (
	sort {
		@ar = split("\t", $a);
		$valA = pop @ar;
		$valA =~ s/[~<> ]+//g;
		@ar = split("\t", $b);
		$valB = pop @ar;
		$valB =~ s/[~<> ]+//g;
		$valA <=> $valB
	} @rows)
{
	print "$_\n";
}
