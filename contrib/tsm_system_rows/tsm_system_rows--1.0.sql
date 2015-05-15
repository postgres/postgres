/* src/test/modules/tablesample/tsm_system_rows--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION tsm_system_rows" to load this file. \quit

CREATE FUNCTION tsm_system_rows_init(internal, int4, int4)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION tsm_system_rows_nextblock(internal)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION tsm_system_rows_nexttuple(internal, int4, int2)
RETURNS int2
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION tsm_system_rows_examinetuple(internal, int4, internal, bool)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION tsm_system_rows_end(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION tsm_system_rows_reset(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION tsm_system_rows_cost(internal, internal, internal, internal, internal, internal, internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

INSERT INTO pg_tablesample_method VALUES('system_rows', false, true,
	'tsm_system_rows_init', 'tsm_system_rows_nextblock',
	'tsm_system_rows_nexttuple', 'tsm_system_rows_examinetuple',
	'tsm_system_rows_end', 'tsm_system_rows_reset', 'tsm_system_rows_cost');

