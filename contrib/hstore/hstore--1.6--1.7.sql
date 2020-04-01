/* contrib/hstore/hstore--1.6--1.7.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION hstore UPDATE TO '1.7'" to load this file. \quit

CREATE FUNCTION ghstore_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', 'ghstore_options'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

ALTER OPERATOR FAMILY gist_hstore_ops USING gist
ADD FUNCTION 10 (hstore) ghstore_options (internal);

ALTER OPERATOR ? (hstore, text)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ?| (hstore, text[])
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ?& (hstore, text[])
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR @> (hstore, hstore)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR <@ (hstore, hstore)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR @ (hstore, hstore)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ~ (hstore, hstore)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
