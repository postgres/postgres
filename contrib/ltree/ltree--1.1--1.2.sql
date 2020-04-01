/* contrib/ltree/ltree--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION ltree UPDATE TO '1.2'" to load this file. \quit

CREATE FUNCTION ltree_recv(internal)
RETURNS ltree
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION ltree_send(ltree)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

ALTER TYPE ltree SET ( RECEIVE = ltree_recv, SEND = ltree_send );

CREATE FUNCTION lquery_recv(internal)
RETURNS lquery
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION lquery_send(lquery)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

ALTER TYPE lquery SET ( RECEIVE = lquery_recv, SEND = lquery_send );

CREATE FUNCTION ltxtq_recv(internal)
RETURNS ltxtquery
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION ltxtq_send(ltxtquery)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

ALTER TYPE ltxtquery SET ( RECEIVE = ltxtq_recv, SEND = ltxtq_send );


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
