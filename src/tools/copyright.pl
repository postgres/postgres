#!/usr/bin/perl
#################################################################
# copyright.pl -- update copyright notices throughout the source tree, idempotently.
#
# Copyright (c) 2011-2012, PostgreSQL Global Development Group
#
# src/tools/copyright.pl
#################################################################

use strict;
use warnings;

use File::Find;
use Tie::File;

my $pgdg = 'PostgreSQL Global Development Group';
my $cc = 'Copyright \(c\) ';
# year-1900 is what localtime(time) puts in element 5
my $year = 1900 + ${[localtime(time)]}[5];

print "Using current year:  $year\n";

find({wanted => \&wanted, no_chdir => 1}, '.');

sub wanted {
    # prevent corruption of git indexes by ignoring any .git/
    if ($_ eq '.git')
    {
        $File::Find::prune = 1;
        return;
    }

    return if ! -f $File::Find::name || -l $File::Find::name;
    # skip file names with binary extensions
    # How are these updated?  bjm 2012-01-02
    return if ($_ =~ m/\.(ico|bin)$);

    my @lines;
    tie @lines, "Tie::File", $File::Find::name;

    foreach my $line (@lines) {
        # We only care about lines with a copyright notice.
        next unless $line =~ m/$cc.*$pgdg/;
        # We stop when we've done one substitution.  This is both for
        # efficiency and, at least in the case of this program, for
        # correctness.
        last if $line =~ m/$cc.*$year.*$pgdg/;
        last if $line =~ s/($cc\d{4})(, $pgdg)/$1-$year$2/;
        last if $line =~ s/($cc\d{4})-\d{4}(, $pgdg)/$1-$year$2/;
    }
    untie @lines;
}

print "Manually update doc/src/sgml/legal.sgml and src/interfaces/libpq/libpq.rc.in too\n";

