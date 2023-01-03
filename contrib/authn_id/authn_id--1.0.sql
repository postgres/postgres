/* contrib/authn_id/authn_id--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION authn_id" to load this file. \quit

CREATE FUNCTION authn_id() RETURNS text
AS 'MODULE_PATHNAME', 'authn_id'
LANGUAGE C IMMUTABLE;
