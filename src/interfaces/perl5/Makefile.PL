#-------------------------------------------------------
#
# $Id: Makefile.PL,v 1.9 1998/09/27 19:12:21 mergl Exp $
#
# Copyright (c) 1997, 1998  Edmund Mergl
#
#-------------------------------------------------------

use ExtUtils::MakeMaker;
use Config;
use strict;

# because the perl5 interface is always contained in the source tree,
# we can be sure about the location of the include files and libs.
# For development and testing we still test for POSTGRES_HOME.
#
#print "\nConfiguring Pg\n";
#print "Remember to actually read the README file !\n";
#die "\nYou didn't read the README file !\n" unless ($] >= 5.002);
#
#if (! $ENV{POSTGRES_HOME}) {
#    warn "\$POSTGRES_HOME not defined. Searching for PostgreSQL...\n";
#    foreach(qw(../../../ /usr/local/pgsql /usr/pgsql /home/pgsql /opt/pgsql /usr/local/postgres /usr/postgres /home/postgres /opt/postgres)) {
#        if (-d "$_/lib") {
#            $ENV{POSTGRES_HOME} = $_;
#            last;
#        }
#    }
#}
#
#if (-d "$ENV{POSTGRES_HOME}/lib") {
#    print "Found PostgreSQL in $ENV{POSTGRES_HOME}\n";
#} else {
#    die "Unable to determine PostgreSQL\n";
#}

my %opts;

if (! $ENV{POSTGRES_HOME}) {

    my $cwd = `pwd`;
    chop $cwd;

    %opts = (
        NAME         => 'Pg',
        VERSION_FROM => 'Pg.pm',
        INC          => "-I$cwd/../libpq -I$cwd/../../include",
        OBJECT       => "Pg\$(OBJ_EXT)",
        LIBS         => ["-L$cwd/../libpq -lpq"],
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
