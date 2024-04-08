/* src/test/modules/test_tam_options/test_tam_options--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_tam_options" to load this file. \quit

CREATE FUNCTION heap_alter_options_tam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE ACCESS METHOD heap_alter_options TYPE TABLE
HANDLER heap_alter_options_tam_handler;
