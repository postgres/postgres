#! /usr/bin/perl

use strict;
use warnings;
use locale;

open(my $in_fh, '<', $ARGV[0]) || die;
chop(my (@words) = <$in_fh>);
close($in_fh);

$" = "\n";
my (@result) = sort @words;

print "@result\n";
