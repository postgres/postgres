#-------------------------------------------------------
#
# $Id: Makefile.PL,v 1.3 1997/09/17 20:46:20 mergl Exp $
#
# Copyright (c) 1997  Edmund Mergl
#
#-------------------------------------------------------

use ExtUtils::MakeMaker;

print "\nConfiguring Pg\n";
print "Remember to actually read the README file !\n";
die "\nYou didn't read the README file !\n" unless ($] >= 5.003);

if (! $ENV{POSTGRES_HOME}) {
    warn "\$POSTGRES_HOME not defined. Searching for PostgreSQL...\n";
    foreach(qw(/usr/pgsql /usr/local/pgsql /usr/pgsql-6.1 /usr/local/pgsql-6.1)) {
        if (-d "$_/lib") {
            $ENV{POSTGRES_HOME} = $_;
            last;
        }
    }
}

if ($ENV{POSTGRES_HOME}) {
    print "\nFound PostgreSQL in $ENV{POSTGRES_HOME}\n";
} else {
    die "Unable to determine \$POSTGRES_HOME !\n";
}

WriteMakefile(
    'NAME'	   => 'Pg',
    'VERSION_FROM' => 'Pg.pm',
    'LIBS'	   => ["-L$ENV{POSTGRES_HOME}/lib -lpq"],
    'INC'	   =>  "-I$ENV{POSTGRES_HOME}/include",
);

# EOF
