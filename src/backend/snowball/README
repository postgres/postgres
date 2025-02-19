src/backend/snowball/README

Snowball-Based Stemming
=======================

This module uses the word stemming code developed by the Snowball project,
http://snowballstem.org (formerly http://snowball.tartarus.org)
which is released by them under a BSD-style license.

The Snowball project does not often make formal releases; it's best
to pull from their git repository

git clone https://github.com/snowballstem/snowball.git

and then building the derived files is as simple as

cd snowball
make

At least on Linux, no platform-specific adjustment is needed.

Postgres' files under src/backend/snowball/libstemmer/ and
src/include/snowball/libstemmer/ are taken directly from the Snowball
files, with only some minor adjustments of file inclusions.  Note
that most of these files are in fact derived files, not original source.
The original sources are in the Snowball language, and are built using
the Snowball-to-C compiler that is also part of the Snowball project.
We choose to include the derived files in the PostgreSQL distribution
because most installations will not have the Snowball compiler available.

We are currently synced with the Snowball git commit
d19326ac6c1b9a417fc872f7c2f845265a5e9ece
of 2025-02-19.

To update the PostgreSQL sources from a new Snowball version:

0. If you didn't do it already, "make -C snowball".

1. Copy the *.c files in snowball/src_c/ to src/backend/snowball/libstemmer
with replacement of "../runtime/header.h" by "header.h", for example

for f in .../snowball/src_c/*.c
do
    sed 's|\.\./runtime/header\.h|header.h|' $f >libstemmer/`basename $f`
done

Do not copy stemmers that are listed in their libstemmer/modules.txt as
nonstandard, such as "kraaij_pohlmann" or "lovins".

2. Copy the *.c files in snowball/runtime/ to
src/backend/snowball/libstemmer, and edit them to remove direct inclusions
of system headers such as <stdio.h> --- they should only include "header.h".
(This removal avoids portability problems on some platforms where <stdio.h>
is sensitive to largefile compilation options.)

3. Copy the *.h files in snowball/src_c/ and snowball/runtime/
to src/include/snowball/libstemmer.  At this writing the header files
do not require any changes.  Again, omit the *.h files for nonstandard
stemmers.

4. Check whether any stemmer modules have been added or removed.  If so, edit
the OBJS list in Makefile, the dict_snowball_sources list in meson.build,
the list of #include's and the stemmer_modules[] table in dict_snowball.c,
and the sample \dFd output in the documentation in textsearch.sgml.
You might also need to change the @languages array in snowball_create.pl
and the tsearch_config_languages[] table in initdb.c.

5. The various stopword files in stopwords/ must be downloaded
individually from pages on the snowballstem.org website.
Be careful that these files must be stored in UTF-8 encoding.
Update the stop_files list in Makefile if any are added or removed
(the meson tooling does not require adjustment for that, though).
