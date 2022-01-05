/*
 * This test is for Linux/glibc systems (conceivably it could be run on
 * others that implement proper classification of high Unicode characters).
 * It must be run in a database with UTF8 encoding and a Unicode-aware locale.
 */

SELECT getdatabaseencoding() <> 'UTF8' OR
       current_setting('lc_ctype') = 'C' OR
       version() !~ 'linux-gnu'
       AS skip_test \gset
\if :skip_test
\quit
\endif

SET client_encoding TO UTF8;

--
-- Test the "high colormap" logic with single characters and ranges that
-- exceed the MAX_SIMPLE_CHR cutoff, here assumed to be less than U+2000.
--

-- trivial cases:
SELECT 'aⓐ' ~ U&'a\24D0' AS t;
SELECT 'aⓐ' ~ U&'a\24D1' AS f;
SELECT 'aⓕ' ~ 'a[ⓐ-ⓩ]' AS t;
SELECT 'aⒻ' ~ 'a[ⓐ-ⓩ]' AS f;
-- cases requiring splitting of ranges:
SELECT 'aⓕⓕ' ~ 'aⓕ[ⓐ-ⓩ]' AS t;
SELECT 'aⓕⓐ' ~ 'aⓕ[ⓐ-ⓩ]' AS t;
SELECT 'aⓐⓕ' ~ 'aⓕ[ⓐ-ⓩ]' AS f;
SELECT 'aⓕⓕ' ~ 'a[ⓐ-ⓩ]ⓕ' AS t;
SELECT 'aⓕⓐ' ~ 'a[ⓐ-ⓩ]ⓕ' AS f;
SELECT 'aⓐⓕ' ~ 'a[ⓐ-ⓩ]ⓕ' AS t;
SELECT 'aⒶⓜ' ~ 'a[Ⓐ-ⓜ][ⓜ-ⓩ]' AS t;
SELECT 'aⓜⓜ' ~ 'a[Ⓐ-ⓜ][ⓜ-ⓩ]' AS t;
SELECT 'aⓜⓩ' ~ 'a[Ⓐ-ⓜ][ⓜ-ⓩ]' AS t;
SELECT 'aⓩⓩ' ~ 'a[Ⓐ-ⓜ][ⓜ-ⓩ]' AS f;
SELECT 'aⓜ⓪' ~ 'a[Ⓐ-ⓜ][ⓜ-ⓩ]' AS f;
SELECT 'a0' ~ 'a[a-ⓩ]' AS f;
SELECT 'aq' ~ 'a[a-ⓩ]' AS t;
SELECT 'aⓜ' ~ 'a[a-ⓩ]' AS t;
SELECT 'a⓪' ~ 'a[a-ⓩ]' AS f;

-- Locale-dependent character classes

SELECT 'aⒶⓜ⓪' ~ '[[:alpha:]][[:alpha:]][[:alpha:]][[:graph:]]' AS t;
SELECT 'aⒶⓜ⓪' ~ '[[:alpha:]][[:alpha:]][[:alpha:]][[:alpha:]]' AS f;

-- Locale-dependent character classes with high ranges

SELECT 'aⒶⓜ⓪' ~ '[a-z][[:alpha:]][ⓐ-ⓩ][[:graph:]]' AS t;
SELECT 'aⓜⒶ⓪' ~ '[a-z][[:alpha:]][ⓐ-ⓩ][[:graph:]]' AS f;
SELECT 'aⓜⒶ⓪' ~ '[a-z][ⓐ-ⓩ][[:alpha:]][[:graph:]]' AS t;
SELECT 'aⒶⓜ⓪' ~ '[a-z][ⓐ-ⓩ][[:alpha:]][[:graph:]]' AS f;
