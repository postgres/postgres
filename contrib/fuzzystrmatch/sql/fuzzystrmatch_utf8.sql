/*
 * This test must be run in a database with UTF-8 encoding,
 * because other encodings don't support all the characters used.
 */

SELECT getdatabaseencoding() <> 'UTF8'
       AS skip_test \gset
\if :skip_test
\quit
\endif

set client_encoding = utf8;

-- CREATE EXTENSION IF NOT EXISTS fuzzystrmatch;

-- Accents
SELECT daitch_mokotoff('Müller');
SELECT daitch_mokotoff('Schäfer');
SELECT daitch_mokotoff('Straßburg');
SELECT daitch_mokotoff('Éregon');

-- Special characters added at https://www.jewishgen.org/InfoFiles/Soundex.html
SELECT daitch_mokotoff('gąszczu');
SELECT daitch_mokotoff('brzęczy');
SELECT daitch_mokotoff('ţamas');
SELECT daitch_mokotoff('țamas');
