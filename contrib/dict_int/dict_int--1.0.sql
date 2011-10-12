/* contrib/dict_int/dict_int--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION dict_int" to load this file. \quit

CREATE FUNCTION dintdict_init(internal)
        RETURNS internal
        AS 'MODULE_PATHNAME'
        LANGUAGE C STRICT;

CREATE FUNCTION dintdict_lexize(internal, internal, internal, internal)
        RETURNS internal
        AS 'MODULE_PATHNAME'
        LANGUAGE C STRICT;

CREATE TEXT SEARCH TEMPLATE intdict_template (
        LEXIZE = dintdict_lexize,
	INIT   = dintdict_init
);

CREATE TEXT SEARCH DICTIONARY intdict (
	TEMPLATE = intdict_template
);

COMMENT ON TEXT SEARCH DICTIONARY intdict IS 'dictionary for integers';
