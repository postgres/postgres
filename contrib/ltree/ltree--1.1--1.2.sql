/* contrib/ltree/ltree--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION ltree UPDATE TO '1.2'" to load this file. \quit

CREATE FUNCTION ltree_gist_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', 'ltree_gist_options'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION _ltree_gist_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', '_ltree_gist_options'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

ALTER OPERATOR FAMILY gist_ltree_ops USING gist
ADD FUNCTION 10 (ltree) ltree_gist_options (internal);

ALTER OPERATOR FAMILY gist__ltree_ops USING gist
ADD FUNCTION 10 (_ltree) _ltree_gist_options (internal);

ALTER OPERATOR < (ltree, ltree)
  SET (RESTRICT = scalarltsel, JOIN = scalarltjoinsel);
ALTER OPERATOR <= (ltree, ltree)
  SET (RESTRICT = scalarlesel, JOIN = scalarlejoinsel);
ALTER OPERATOR >= (ltree, ltree)
  SET (RESTRICT = scalargesel, JOIN = scalargejoinsel);
ALTER OPERATOR > (ltree, ltree)
  SET (RESTRICT = scalargtsel, JOIN = scalargtjoinsel);

ALTER OPERATOR @> (ltree, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^@> (ltree, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR <@ (ltree, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^<@ (ltree, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ~ (ltree, lquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ~ (lquery, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^~ (ltree, lquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^~ (lquery, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ? (ltree, _lquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ? (_lquery, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^? (ltree, _lquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^? (_lquery, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR @ (ltree, ltxtquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR @ (ltxtquery, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^@ (ltree, ltxtquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^@ (ltxtquery, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR @> (_ltree, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR <@ (ltree, _ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR <@ (_ltree, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR @> (ltree, _ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ~ (_ltree, lquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ~ (lquery, _ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ? (_ltree, _lquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ? (_lquery, _ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR @ (_ltree, ltxtquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR @ (ltxtquery, _ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^@> (_ltree, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^<@ (ltree, _ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^<@ (_ltree, ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^@> (ltree, _ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^~ (_ltree, lquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^~ (lquery, _ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^? (_ltree, _lquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^? (_lquery, _ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^@ (_ltree, ltxtquery)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
ALTER OPERATOR ^@ (ltxtquery, _ltree)
  SET (RESTRICT = matchingsel, JOIN = matchingjoinsel);
