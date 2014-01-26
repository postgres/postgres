#
# Script that provides 'make install' functionality for msvc builds
#
# src/tools/msvc/install.pl
#
use strict;
use warnings;

use Install qw(Install);

my $target = shift || Usage();
my $insttype = shift;
Install($target, $insttype);

sub Usage
{
	print "Usage: install.pl <targetdir> [installtype]\n";
	print "installtype: client\n";
	exit(1);
}
