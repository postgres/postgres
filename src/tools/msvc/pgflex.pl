# -*-perl-*- hey - emacs - this is a perl file

# src/tools/msvc/pgflex.pl

# silence flex bleatings about file path style
$ENV{CYGWIN} = 'nodosfilewarning';

use strict;
use File::Basename;

# assume we are in the postgres source root

require 'src/tools/msvc/buildenv.pl' if -e 'src/tools/msvc/buildenv.pl';

system('flex -V > NUL');
if ($? != 0)
{
    print "WARNING! flex install not found, attempting to build without\n";
    exit 0;
}

my $input = shift;
if ($input !~ /\.l$/)
{
    print "Input must be a .l file\n";
    exit 1;
}
elsif (!-e $input)
{
    print "Input file $input not found\n";
    exit 1;
}

(my $output = $input) =~ s/\.l$/.c/;

# get flex flags from make file
my $makefile = dirname($input) . "/Makefile";
my ($mf, $make);
open($mf,$makefile);
local $/ = undef;
$make=<$mf>;
close($mf);
my $flexflags = ($make =~ /^\s*FLEXFLAGS\s*=\s*(\S.*)/m ? $1 : '');

system("flex $flexflags -o$output $input");
if ($? == 0)
{

    # For non-reentrant scanners we need to fix up the yywrap macro definition
    # to keep the MS compiler happy.
    # For reentrant scanners (like the core scanner) we do not
    # need to (and must not) change the yywrap definition.
    my $lfile;
    open($lfile,$input) || die "opening $input for reading: $!";
    my $lcode = <$lfile>;
    close($lfile);
    if ($lcode !~ /\%option\sreentrant/)
    {
        my $cfile;
        open($cfile,$output) || die "opening $output for reading: $!";
        my $ccode = <$cfile>;
        close($cfile);
        $ccode =~ s/yywrap\(n\)/yywrap()/;
        open($cfile,">$output") || die "opening $output for reading: $!";
        print $cfile $ccode;
        close($cfile);
    }

    exit 0;

}
else
{
    exit $? >> 8;
}

