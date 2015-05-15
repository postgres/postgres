/* src/test/modules/tablesample/tsm_system_time--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION tsm_system_time" to load this file. \quit

CREATE FUNCTION tsm_system_time_init(internal, int4, int4)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION tsm_system_time_nextblock(internal)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION tsm_system_time_nexttuple(internal, int4, int2)
RETURNS int2
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION tsm_system_time_end(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION tsm_system_time_reset(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION tsm_system_time_cost(internal, internal, internal, internal, internal, internal, internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

INSERT INTO pg_tablesample_method VALUES('system_time', false, true,
	'tsm_system_time_init', 'tsm_system_time_nextblock',
	'tsm_system_time_nexttuple', '-', 'tsm_system_time_end',
	'tsm_system_time_reset', 'tsm_system_time_cost');

