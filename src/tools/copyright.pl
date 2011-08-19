#!/usr/bin/perl 
#################################################################
# copyright.pl -- update copyright notices throughout the source tree, idempotently.
#
# Copyright (c) 2011, PostgreSQL Global Development Group
#
# src/tools/copyright.pl
#################################################################

use strict;
use warnings;

use File::Find;

my $pgdg = 'PostgreSQL Global Development Group';
my $cc = 'Copyright \(c\) ';
# year-1900 is what localtime(time) puts in element 5
my $year = 1900 + ${[localtime(time)]}[5];

print "Using current year:  $year\n";

find({wanted => \&wanted, no_chdir => 1}, '.');

sub wanted {
    my $filename = $File::Find::name;

    # only regular files
    return if ! -f $filename;

    open(my $FILE, '<', $filename) or die "Cannot open $filename";

    foreach my $line (<$FILE>) {
        # We only care about lines with a copyright notice.
        next unless $line =~ m/$cc.*$pgdg/;
        # We stop when we've done one substitution.  This is both for
        # efficiency and, at least in the case of this program, for
        # correctness.
        last if $line =~ m/$cc.*$year.*$pgdg/;
        last if $line =~ s/($cc\d{4})(, $pgdg)/$1-$year$2/;
        last if $line =~ s/($cc\d{4})-\d{4}(, $pgdg)/$1-$year$2/;
    }
    close($FILE) or die "Cannot close $filename";
}

print "Manually update doc/src/sgml/legal.sgml and src/interfaces/libpq/libpq.rc.in too\n";

