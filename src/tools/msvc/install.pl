#
# Script that provides 'make install' functionality for msvc builds
#
# $PostgreSQL: pgsql/src/tools/msvc/install.pl,v 1.7 2007/03/17 14:01:01 mha Exp $
#
use strict;
use warnings;

use Install qw(Install);

my $target = shift || Usage();
Install($target);

sub Usage
{
    print "Usage: install.pl <targetdir>\n";
    exit(1);
}
