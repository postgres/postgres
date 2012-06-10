#
# Script that provides 'make install' functionality for msvc builds
#
# src/tools/msvc/install.pl
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
