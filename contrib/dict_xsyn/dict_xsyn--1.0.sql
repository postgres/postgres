/* contrib/dict_xsyn/dict_xsyn--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION dict_xsyn" to load this file. \quit

CREATE FUNCTION dxsyn_init(internal)
        RETURNS internal
        AS 'MODULE_PATHNAME'
        LANGUAGE C STRICT;

CREATE FUNCTION dxsyn_lexize(internal, internal, internal, internal)
        RETURNS internal
        AS 'MODULE_PATHNAME'
        LANGUAGE C STRICT;

CREATE TEXT SEARCH TEMPLATE xsyn_template (
        LEXIZE = dxsyn_lexize,
	INIT   = dxsyn_init
);

CREATE TEXT SEARCH DICTIONARY xsyn (
	TEMPLATE = xsyn_template
);

COMMENT ON TEXT SEARCH DICTIONARY xsyn IS 'eXtended synonym dictionary';
