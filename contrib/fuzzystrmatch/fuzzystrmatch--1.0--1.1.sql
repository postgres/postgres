/* contrib/fuzzystrmatch/fuzzystrmatch--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION fuzzystrmatch UPDATE TO '1.1'" to load this file. \quit

ALTER FUNCTION levenshtein(text, text) PARALLEL SAFE;
ALTER FUNCTION levenshtein(text, text, int, int, int) PARALLEL SAFE;
ALTER FUNCTION levenshtein_less_equal(text, text, int) PARALLEL SAFE;
ALTER FUNCTION levenshtein_less_equal(text, text, int, int, int, int) PARALLEL SAFE;
ALTER FUNCTION metaphone(text, int) PARALLEL SAFE;
ALTER FUNCTION soundex(text) PARALLEL SAFE;
ALTER FUNCTION text_soundex(text) PARALLEL SAFE;
ALTER FUNCTION difference(text, text) PARALLEL SAFE;
ALTER FUNCTION dmetaphone(text) PARALLEL SAFE;
ALTER FUNCTION dmetaphone_alt(text) PARALLEL SAFE;
