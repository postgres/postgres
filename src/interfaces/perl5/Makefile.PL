# Generated automatically from Makefile.PL.in by configure.
#-------------------------------------------------------
#
# $Id: Makefile.PL,v 1.12 1998/10/17 02:36:13 momjian Exp $
#
# Copyright (c) 1997, 1998  Edmund Mergl
#
#-------------------------------------------------------

use ExtUtils::MakeMaker;
use Config;
use strict;

my %opts;

if (! $ENV{POSTGRES_HOME}) {

    my $cwd = `pwd`;
    chop $cwd;

    %opts = (
        NAME         => 'Pg',
        VERSION_FROM => 'Pg.pm',
        INC          => "-I$cwd/../libpq -I$cwd/../../include",
        OBJECT       => "Pg\$(OBJ_EXT)",
        LIBS         => ["-L/usr/local/pgsql/lib -L$cwd/../libpq -lpq"],
    );

} else {

    %opts = (
        NAME         => 'Pg',
        VERSION_FROM => 'Pg.pm',
        INC          => "-I$ENV{POSTGRES_HOME}/include",
        OBJECT       => "Pg\$(OBJ_EXT)",
        LIBS         => ["-L$ENV{POSTGRES_HOME}/lib -lpq"],
    );
}


WriteMakefile(%opts);

exit(0);

# end of Makefile.PL
