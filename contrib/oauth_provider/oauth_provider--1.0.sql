/* contrib/oauth_provider/oauth_provider--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION oauth_token_validator" to load this file. \quit

CREATE FUNCTION oauth_provider() RETURNS text
AS 'MODULE_PATHNAME', 'oauth_provider'
LANGUAGE C IMMUTABLE;
