#-------------------------------------------------------
#
# $Id: Makefile.PL,v 1.6 1998/04/06 01:36:30 momjian Exp $
#
# Copyright (c) 1997  Edmund Mergl
#
#-------------------------------------------------------

use ExtUtils::MakeMaker;
use Config;
use strict;

print "\nConfiguring Pg\n";
print "Remember to actually read the README file !\n";
die "\nYou didn't read the README file !\n" unless ($] >= 5.002);

my %opts = (
    NAME         => 'Pg',
    VERSION_FROM => 'Pg.pm',
    INC	         =>  "-I../libpq -I../../include",
    LIBS         => ["-L../libpq -lpq"],
    CC		 => 'gcc',
);


WriteMakefile(%opts);

exit(0);

# end of Makefile.PL
