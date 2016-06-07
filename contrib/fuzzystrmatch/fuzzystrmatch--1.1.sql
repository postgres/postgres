/* contrib/fuzzystrmatch/fuzzystrmatch--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION fuzzystrmatch" to load this file. \quit

CREATE FUNCTION levenshtein (text,text) RETURNS int
AS 'MODULE_PATHNAME','levenshtein'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION levenshtein (text,text,int,int,int) RETURNS int
AS 'MODULE_PATHNAME','levenshtein_with_costs'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION levenshtein_less_equal (text,text,int) RETURNS int
AS 'MODULE_PATHNAME','levenshtein_less_equal'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION levenshtein_less_equal (text,text,int,int,int,int) RETURNS int
AS 'MODULE_PATHNAME','levenshtein_less_equal_with_costs'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION metaphone (text,int) RETURNS text
AS 'MODULE_PATHNAME','metaphone'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION soundex(text) RETURNS text
AS 'MODULE_PATHNAME', 'soundex'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION text_soundex(text) RETURNS text
AS 'MODULE_PATHNAME', 'soundex'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION difference(text,text) RETURNS int
AS 'MODULE_PATHNAME', 'difference'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION dmetaphone (text) RETURNS text
AS 'MODULE_PATHNAME', 'dmetaphone'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION dmetaphone_alt (text) RETURNS text
AS 'MODULE_PATHNAME', 'dmetaphone_alt'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
