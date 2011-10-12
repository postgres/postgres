/* contrib/spi/moddatetime--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION moddatetime" to load this file. \quit

CREATE FUNCTION moddatetime()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;
