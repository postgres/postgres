#-------------------------------------------------------
#
# $Id: Makefile.PL,v 1.10 1998/10/16 04:37:38 momjian Exp $
#
# Copyright (c) 1997, 1998  Edmund Mergl
#
#-------------------------------------------------------

use ExtUtils::MakeMaker;
use Config;
use strict;

# This Makefile.PL is intended for standalone use when PostgreSQL is
# already installed.  In that case, install the perl module as follows:
# 
#    setenv POSTGRES_HOME /path/to/root/of/installed/postgres
#    perl Makefile.PL
#    make
#    make test
#    make install

# During normal installation of PostgreSQL, this file will be replaced
# by one derived from Makefile.PL.in so that the installed shared
# library libpq.so will be found during installation of this module.
# As a result, the POSTGRES_HOME environment variable need not be set
# during PostgreSQL installation.  Note that ../Makefile takes care of
# the `perl Makefile.PL' command.  Note also that it is still possible
# to follow the standalone installation procedure, even after
# configuring and installing PostgreSQL, because the `else'
# conditional branch below is identical in both Makefile.PL and
# Makefile.PL.in.

my %opts;

if (! $ENV{POSTGRES_HOME}) {

    my $cwd = `pwd`;
    chop $cwd;

    print "To install the perl interface for PostgreSQL do the following:\n";
    print "  - install PostgreSQL\n";
    print "  - set the POSTGRES_HOME environment variable appropriately\n";
    print "  - in this directory ($cwd):\n";
    print "    perl Makefile.PL\n";
    print "    make\n";
    print "    make test	[ with a postmaster running ]\n";
    print "    make install\n";

    exit(1);
  
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
