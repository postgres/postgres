#! /usr/bin/perl
use locale;

open(INFILE, "<$ARGV[0]");
chop(my(@words) = <INFILE>);
close(INFILE);

$"="\n";
my(@result) = sort @words;

print "@result\n";
