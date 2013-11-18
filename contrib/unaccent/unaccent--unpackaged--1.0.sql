/* contrib/unaccent/unaccent--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION unaccent" to load this file. \quit

ALTER EXTENSION unaccent ADD function unaccent(regdictionary,text);
ALTER EXTENSION unaccent ADD function unaccent(text);
ALTER EXTENSION unaccent ADD function unaccent_init(internal);
ALTER EXTENSION unaccent ADD function unaccent_lexize(internal,internal,internal,internal);
ALTER EXTENSION unaccent ADD text search template unaccent;
ALTER EXTENSION unaccent ADD text search dictionary unaccent;

-- These functions are marked as stable in 9.1, were not before:

ALTER FUNCTION unaccent(regdictionary, text) STABLE;
ALTER FUNCTION unaccent(text) STABLE;
