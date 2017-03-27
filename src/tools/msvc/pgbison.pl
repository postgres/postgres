# -*-perl-*- hey - emacs - this is a perl file

# src/tools/msvc/pgbison.pl

use strict;
use File::Basename;

# assume we are in the postgres source root

do 'src/tools/msvc/buildenv.pl' if -e 'src/tools/msvc/buildenv.pl';

my ($bisonver) = `bison -V`;    # grab first line
$bisonver = (split(/\s+/, $bisonver))[3];    # grab version number

unless ($bisonver eq '1.875' || $bisonver ge '2.2')
{
	print "WARNING! Bison install not found, or unsupported Bison version.\n";
	print "echo Attempting to build without.\n";
	exit 0;
}

my $input = shift;
if ($input !~ /\.y$/)
{
	print "Input must be a .y file\n";
	exit 1;
}
elsif (!-e $input)
{
	print "Input file $input not found\n";
	exit 1;
}

(my $output = $input) =~ s/\.y$/.c/;

# plpgsql just has to be different
$output =~ s/gram\.c$/pl_gram.c/ if $input =~ /src.pl.plpgsql.src.gram\.y$/;

my $makefile = dirname($input) . "/Makefile";
my ($mf, $make);
open($mf, '<', $makefile);
local $/ = undef;
$make = <$mf>;
close($mf);
my $basetarg = basename($output);
my $headerflag = ($make =~ /^$basetarg:\s+BISONFLAGS\b.*-d/m ? '-d' : '');

system("bison $headerflag $input -o $output");
exit $? >> 8;
