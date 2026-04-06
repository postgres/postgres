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

-- Non-ASCII stash names should be rejected.
SELECT pg_create_advice_stash('café');
SET pg_stash_advice.stash_name = 'café';
