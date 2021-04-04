/* src/test/modules/spgist_name_ops/spgist_name_ops--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION spgist_name_ops" to load this file. \quit

CREATE FUNCTION spgist_name_config(internal, internal)
RETURNS void IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spgist_name_choose(internal, internal)
RETURNS void IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spgist_name_inner_consistent(internal, internal)
RETURNS void IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spgist_name_leaf_consistent(internal, internal)
RETURNS boolean IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spgist_name_compress(name)
RETURNS text IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE OPERATOR CLASS name_ops DEFAULT FOR TYPE name
USING spgist AS
	OPERATOR	1	< ,
	OPERATOR	2	<= ,
	OPERATOR	3	= ,
	OPERATOR	4	>= ,
	OPERATOR	5	> ,
	FUNCTION	1	spgist_name_config(internal, internal),
	FUNCTION	2	spgist_name_choose(internal, internal),
	FUNCTION	3	spg_text_picksplit(internal, internal),
	FUNCTION	4	spgist_name_inner_consistent(internal, internal),
	FUNCTION	5	spgist_name_leaf_consistent(internal, internal),
	FUNCTION	6	spgist_name_compress(name),
	STORAGE text;

-- Also test old-style where the STORAGE clause is disallowed
CREATE OPERATOR CLASS name_ops_old FOR TYPE name
USING spgist AS
	OPERATOR	1	< ,
	OPERATOR	2	<= ,
	OPERATOR	3	= ,
	OPERATOR	4	>= ,
	OPERATOR	5	> ,
	FUNCTION	1	spgist_name_config(internal, internal),
	FUNCTION	2	spgist_name_choose(internal, internal),
	FUNCTION	3	spg_text_picksplit(internal, internal),
	FUNCTION	4	spgist_name_inner_consistent(internal, internal),
	FUNCTION	5	spgist_name_leaf_consistent(internal, internal),
	FUNCTION	6	spgist_name_compress(name);
