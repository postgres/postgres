/* src/test/modules/test_extensible/test_extensible--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_extensible" to load this file. \quit

CREATE FUNCTION test_get_extensible_node_methods(text, bool) RETURNS text
  AS 'MODULE_PATHNAME', 'test_get_extensible_node_methods' LANGUAGE C STRICT;

CREATE FUNCTION test_get_custom_scan_methods(text, bool) RETURNS text
  AS 'MODULE_PATHNAME', 'test_get_custom_scan_methods' LANGUAGE C STRICT;

CREATE FUNCTION test_ext_node_make(oid, int) RETURNS text
  AS 'MODULE_PATHNAME', 'test_ext_node_make' LANGUAGE C STRICT;

CREATE FUNCTION test_ext_node_copy(text) RETURNS text
  AS 'MODULE_PATHNAME', 'test_ext_node_copy' LANGUAGE C STRICT;

CREATE FUNCTION test_ext_node_equal(text, text) RETURNS bool
  AS 'MODULE_PATHNAME', 'test_ext_node_equal' LANGUAGE C STRICT;

CREATE FUNCTION test_ext_node_get_relid(text) RETURNS oid
  AS 'MODULE_PATHNAME', 'test_ext_node_get_relid' LANGUAGE C STRICT;

CREATE FUNCTION test_ext_node_get_repeat_count(text) RETURNS int
  AS 'MODULE_PATHNAME', 'test_ext_node_get_repeat_count' LANGUAGE C STRICT;
