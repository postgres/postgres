#-------------------------------------------------------
#
# $Id: Makefile.PL,v 1.15 2000/04/23 04:26:32 tgl Exp $
#
# Copyright (c) 1997, 1998  Edmund Mergl
#
#-------------------------------------------------------

use ExtUtils::MakeMaker;
use Config;
use strict;

my %opts;

if (! $ENV{POSTGRES_HOME}) {

    # Check that we actually are inside the Postgres source tree
    if (! -d "../libpq") {
	die
"To install Pg separately from the Postgres distribution,
you must set environment variable POSTGRES_HOME to point to
where Postgres is installed (often /usr/local/pgsql).\n";
    }

    # Setup for build/test inside a Postgres source tree

    # Perl may complain if path to libpq isn't absolute
    my $cwd = `pwd`;
    chop $cwd;

    %opts = (
        NAME         => 'Pg',
        VERSION_FROM => 'Pg.pm',
        INC          => "-I../libpq -I../../include",
        OBJECT       => "Pg\$(OBJ_EXT)",
        LIBS         => ["-L$cwd/../libpq -lpq"],
    );

} else {

    # Setup for final install of Pg using an already-installed libpq,
    # or for standalone installation when Postgres already is installed.

    %opts = (
        NAME         => 'Pg',
        VERSION_FROM => 'Pg.pm',
        INC          => "-I$ENV{POSTGRES_HOME}/include",
        OBJECT       => "Pg\$(OBJ_EXT)",
        LIBS         => ["-L$ENV{POSTGRES_HOME}/lib -lpq"],
    );
}


WriteMakefile(%opts);


sub MY::installbin { 

q[
# Create a target that interfaces/Makefile can use to
# determine the Perl install directory.
echo-installdir:
	@echo $(INSTALLSITELIB)
];

}
