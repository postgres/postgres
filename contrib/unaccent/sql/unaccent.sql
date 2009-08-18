SET client_min_messages = warning;
\set ECHO none
\i unaccent.sql
\set ECHO all
RESET client_min_messages;

-- must have a UTF8 database
SELECT getdatabaseencoding();

SET client_encoding TO 'KOI8';

SELECT unaccent('foobar');
SELECT unaccent('£ΜΛΑ');
SELECT unaccent('³φιλ');

SELECT unaccent('unaccent', 'foobar');
SELECT unaccent('unaccent', '£ΜΛΑ');
SELECT unaccent('unaccent', '³φιλ');

SELECT ts_lexize('unaccent', 'foobar');
SELECT ts_lexize('unaccent', '£ΜΛΑ');
SELECT ts_lexize('unaccent', '³φιλ');
