/* contrib/fuzzystrmatch/fuzzystrmatch--unpackaged--1.0.sql */

ALTER EXTENSION fuzzystrmatch ADD function levenshtein(text,text);
ALTER EXTENSION fuzzystrmatch ADD function levenshtein(text,text,integer,integer,integer);
ALTER EXTENSION fuzzystrmatch ADD function levenshtein_less_equal(text,text,integer);
ALTER EXTENSION fuzzystrmatch ADD function levenshtein_less_equal(text,text,integer,integer,integer,integer);
ALTER EXTENSION fuzzystrmatch ADD function metaphone(text,integer);
ALTER EXTENSION fuzzystrmatch ADD function soundex(text);
ALTER EXTENSION fuzzystrmatch ADD function text_soundex(text);
ALTER EXTENSION fuzzystrmatch ADD function difference(text,text);
ALTER EXTENSION fuzzystrmatch ADD function dmetaphone(text);
ALTER EXTENSION fuzzystrmatch ADD function dmetaphone_alt(text);
