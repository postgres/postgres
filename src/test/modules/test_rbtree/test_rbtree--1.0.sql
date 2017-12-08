/* src/test/modules/test_rbtree/test_rbtree--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_rbtree" to load this file. \quit

CREATE FUNCTION test_rb_tree(size INTEGER)
	RETURNS pg_catalog.void STRICT
	AS 'MODULE_PATHNAME' LANGUAGE C;
