/* contrib/pg_trgm/pg_trgm--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_trgm UPDATE TO '1.5'" to load this file. \quit

CREATE FUNCTION gtrgm_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', 'gtrgm_options'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

ALTER OPERATOR FAMILY gist_trgm_ops USING gist
ADD FUNCTION 10 (text) gtrgm_options (internal);

ALTER OPERATOR % (text, text)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR <% (text, text)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR %> (text, text)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR <<% (text, text)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR %>> (text, text)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
