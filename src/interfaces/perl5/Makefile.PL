#-------------------------------------------------------
#
# $Id: Makefile.PL,v 1.7 1998/04/14 21:14:34 mergl Exp $
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

if (! $ENV{POSTGRES_HOME}) {
    warn "\$POSTGRES_HOME not defined. Searching for PostgreSQL...\n";
    foreach(qw(/usr/local/pgsql /usr/pgsql /home/pgsql /opt/pgsql /usr/local/postgres /usr/postgres /home/postgres /opt/postgres)) {
        if (-d "$_/lib") {
            $ENV{POSTGRES_HOME} = $_;
            last;
        }
    }
}

if (-d "$ENV{POSTGRES_HOME}/lib") {
    print "Found PostgreSQL in $ENV{POSTGRES_HOME}\n";
} else {
    die "Unable to determine PostgreSQL\n";
}

my %opts = (
    NAME         => 'Pg',
    VERSION_FROM => 'Pg.pm',
    INC	         => "-I$ENV{POSTGRES_HOME}/include -I/usr/local/include/pgsql -I/usr/include/pgsql",
    OBJECT       => "Pg\$(OBJ_EXT)",
    LIBS         => ["-L$ENV{POSTGRES_HOME}/lib -lpq"],
);


WriteMakefile(%opts);

exit(0);

# end of Makefile.PL
