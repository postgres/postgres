/*
 * This test must be run in a database with UTF-8 encoding
 * and a Unicode-aware locale.
 */

SELECT getdatabaseencoding() <> 'UTF8' OR
       current_setting('lc_ctype') = 'C'
       AS skip_test \gset
\if :skip_test
\quit
\endif

set client_encoding = utf8;

-- CREATE EXTENSION IF NOT EXISTS citext;

-- Multibyte sanity tests.
SELECT 'À'::citext =  'À'::citext AS t;
SELECT 'À'::citext =  'à'::citext AS t;
SELECT 'À'::text   =  'à'::text   AS f; -- text wins.
SELECT 'À'::citext <> 'B'::citext AS t;

-- Test combining characters making up canonically equivalent strings.
SELECT 'Ä'::text   <> 'Ä'::text   AS t;
SELECT 'Ä'::citext <> 'Ä'::citext AS t;

-- Test the Turkish dotted I. The lowercase is a single byte while the
-- uppercase is multibyte. This is why the comparison code can't be optimized
-- to compare string lengths.
SELECT 'i'::citext = 'İ'::citext AS t;

-- Regression.
SELECT 'láska'::citext <> 'laská'::citext AS t;

SELECT 'Ask Bjørn Hansen'::citext = 'Ask Bjørn Hansen'::citext AS t;
SELECT 'Ask Bjørn Hansen'::citext = 'ASK BJØRN HANSEN'::citext AS t;
SELECT 'Ask Bjørn Hansen'::citext <> 'Ask Bjorn Hansen'::citext AS t;
SELECT 'Ask Bjørn Hansen'::citext <> 'ASK BJORN HANSEN'::citext AS t;
SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'Ask Bjørn Hansen'::citext) = 0 AS t;
SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'ask bjørn hansen'::citext) = 0 AS t;
SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'ASK BJØRN HANSEN'::citext) = 0 AS t;
SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'Ask Bjorn Hansen'::citext) > 0 AS t;
SELECT citext_cmp('Ask Bjorn Hansen'::citext, 'Ask Bjørn Hansen'::citext) < 0 AS t;

-- Test ~<~ and ~<=~
SELECT 'à'::citext ~<~  'À'::citext AS f;
SELECT 'à'::citext ~<=~ 'À'::citext AS t;

-- Test ~>~ and ~>=~
SELECT 'à'::citext ~>~  'À'::citext AS f;
SELECT 'à'::citext ~>=~ 'À'::citext AS t;
