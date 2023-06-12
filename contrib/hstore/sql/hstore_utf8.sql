/*
 * This test must be run in a database with UTF-8 encoding,
 * because other encodings don't support all the characters used.
 */

SELECT getdatabaseencoding() <> 'UTF8'
       AS skip_test \gset
\if :skip_test
\quit
\endif

SET client_encoding = utf8;

-- UTF-8 locale bug on macOS: isspace(0x85) returns true.  \u0105 encodes
-- as 0xc4 0x85 in UTF-8; the 0x85 was interpreted here as a whitespace.
SELECT E'key\u0105=>value\u0105'::hstore;
SELECT 'keyą=>valueą'::hstore;
SELECT 'ą=>ą'::hstore;
SELECT 'keyąfoo=>valueą'::hstore;
