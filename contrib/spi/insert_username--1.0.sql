/* contrib/spi/insert_username--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION insert_username" to load this file. \quit

CREATE FUNCTION insert_username()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;
