/* contrib/unaccent/unaccent--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION unaccent" to load this file. \quit

CREATE FUNCTION unaccent(regdictionary, text)
	RETURNS text
	AS 'MODULE_PATHNAME', 'unaccent_dict'
	LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION unaccent(text)
	RETURNS text
	AS 'MODULE_PATHNAME', 'unaccent_dict'
	LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION unaccent_init(internal)
	RETURNS internal
	AS 'MODULE_PATHNAME', 'unaccent_init'
	LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION unaccent_lexize(internal,internal,internal,internal)
	RETURNS internal
	AS 'MODULE_PATHNAME', 'unaccent_lexize'
	LANGUAGE C PARALLEL SAFE;

CREATE TEXT SEARCH TEMPLATE unaccent (
	INIT = unaccent_init,
	LEXIZE = unaccent_lexize
);

CREATE TEXT SEARCH DICTIONARY unaccent (
	TEMPLATE = unaccent,
	RULES    = 'unaccent'
);
