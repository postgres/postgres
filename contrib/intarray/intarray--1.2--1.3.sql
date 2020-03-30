/* contrib/intarray/intarray--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION intarray UPDATE TO '1.3'" to load this file. \quit

CREATE FUNCTION g_int_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', 'g_int_options'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION g_intbig_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', 'g_intbig_options'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

ALTER OPERATOR FAMILY gist__int_ops USING gist
ADD FUNCTION 10 (_int4) g_int_options (internal);

ALTER OPERATOR FAMILY gist__intbig_ops USING gist
ADD FUNCTION 10 (_int4) g_intbig_options (internal);
