#-------------------------------------------------------
#
# $Id: Makefile.PL,v 1.2 1997/06/02 19:41:59 mergl Exp $
#
# Copyright (c) 1997  Edmund Mergl
#
#-------------------------------------------------------

use ExtUtils::MakeMaker;

print "\nConfiguring Pg\n";
print "Remember to actually read the README file !\n";
die "\nYou didn't read the README file !\n" unless ($] >= 5.003);

if (! $ENV{POSTGRESHOME}) {
    warn "\$POSTGRESHOME not defined. Searching for Postgres...\n";
    foreach(qw(/usr/pgsql /usr/local/pgsql /usr/pgsql-6.1 /usr/local/pgsql-6.1)) {
        if (-d "$_/lib") {
            $ENV{POSTGRESHOME} = $_;
            last;
        }
    }
}

if ($ENV{POSTGRESHOME}) {
    print "\nFound Postgres in $ENV{POSTGRESHOME}\n";
} else {
    die "Unable to determine \$POSTGRESHOME !\n";
}

WriteMakefile(
    'NAME'	   => 'Pg',
    'VERSION_FROM' => 'Pg.pm',
    'LIBS'	   => ["-L$ENV{POSTGRESHOME}/lib -lpq"],
    'INC'	   =>  "-I$ENV{POSTGRESHOME}/include",
);

# EOF
