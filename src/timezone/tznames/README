src/timezone/tznames/README

tznames
=======

This directory contains files with timezone sets for PostgreSQL.  The problem
is that time zone abbreviations are not unique throughout the world and you
might find out that a time zone abbreviation in the `Default' set collides
with the one you wanted to use.  This can be fixed by selecting a timezone
set that defines the abbreviation the way you want it.  There might already
be a file here that serves your needs.  If not, you can create your own.

In order to use one of these files, you need to set

   timezone_abbreviations = 'xyz'

in any of the usual ways for setting a parameter, where xyz is the filename
that contains the desired time zone abbreviations.

If you do not find an appropriate set of abbreviations for your geographic
location supplied here, please report this to <pgsql-hackers@lists.postgresql.org>.
Your set of time zone abbreviations can then be included in future releases.
For the time being you can always add your own set.

Typically a custom abbreviation set is made by including the `Default' set
and then adding or overriding abbreviations as necessary.  For examples,
see the `Australia' and `India' files.

The files named Africa.txt, etc, are not intended to be used directly as
time zone abbreviation files. They contain reference definitions of time zone
abbreviations that can be copied into a custom abbreviation file as needed.
These files contain most of the time zone abbreviations that were shown
in the IANA timezone database circa 2010.

However, it turns out that many of these abbreviations had simply been
invented by the IANA timezone group, and do not have currency in real-world
use.  The IANA group have changed their policy about that, and now prefer to
use numeric UTC offsets whenever there's not an abbreviation with known
real-world popularity.  A lot of these abbreviations therefore no longer
appear in the IANA data, and so are marked "obsolete" in these data files.
