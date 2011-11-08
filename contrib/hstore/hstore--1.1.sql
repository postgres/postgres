/* contrib/hstore/hstore--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION hstore" to load this file. \quit

CREATE TYPE hstore;

CREATE FUNCTION hstore_in(cstring)
RETURNS hstore
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION hstore_out(hstore)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION hstore_recv(internal)
RETURNS hstore
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION hstore_send(hstore)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE TYPE hstore (
        INTERNALLENGTH = -1,
        INPUT = hstore_in,
        OUTPUT = hstore_out,
        RECEIVE = hstore_recv,
        SEND = hstore_send,
        STORAGE = extended
);

CREATE FUNCTION hstore_version_diag(hstore)
RETURNS integer
AS 'MODULE_PATHNAME','hstore_version_diag'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION fetchval(hstore,text)
RETURNS text
AS 'MODULE_PATHNAME','hstore_fetchval'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR -> (
	LEFTARG = hstore,
	RIGHTARG = text,
	PROCEDURE = fetchval
);

CREATE FUNCTION slice_array(hstore,text[])
RETURNS text[]
AS 'MODULE_PATHNAME','hstore_slice_to_array'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR -> (
	LEFTARG = hstore,
	RIGHTARG = text[],
	PROCEDURE = slice_array
);

CREATE FUNCTION slice(hstore,text[])
RETURNS hstore
AS 'MODULE_PATHNAME','hstore_slice_to_hstore'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION isexists(hstore,text)
RETURNS bool
AS 'MODULE_PATHNAME','hstore_exists'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION exist(hstore,text)
RETURNS bool
AS 'MODULE_PATHNAME','hstore_exists'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR ? (
	LEFTARG = hstore,
	RIGHTARG = text,
	PROCEDURE = exist,
	RESTRICT = contsel,
	JOIN = contjoinsel
);

CREATE FUNCTION exists_any(hstore,text[])
RETURNS bool
AS 'MODULE_PATHNAME','hstore_exists_any'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR ?| (
	LEFTARG = hstore,
	RIGHTARG = text[],
	PROCEDURE = exists_any,
	RESTRICT = contsel,
	JOIN = contjoinsel
);

CREATE FUNCTION exists_all(hstore,text[])
RETURNS bool
AS 'MODULE_PATHNAME','hstore_exists_all'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR ?& (
	LEFTARG = hstore,
	RIGHTARG = text[],
	PROCEDURE = exists_all,
	RESTRICT = contsel,
	JOIN = contjoinsel
);

CREATE FUNCTION isdefined(hstore,text)
RETURNS bool
AS 'MODULE_PATHNAME','hstore_defined'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION defined(hstore,text)
RETURNS bool
AS 'MODULE_PATHNAME','hstore_defined'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION delete(hstore,text)
RETURNS hstore
AS 'MODULE_PATHNAME','hstore_delete'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION delete(hstore,text[])
RETURNS hstore
AS 'MODULE_PATHNAME','hstore_delete_array'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION delete(hstore,hstore)
RETURNS hstore
AS 'MODULE_PATHNAME','hstore_delete_hstore'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR - (
	LEFTARG = hstore,
	RIGHTARG = text,
	PROCEDURE = delete
);

CREATE OPERATOR - (
	LEFTARG = hstore,
	RIGHTARG = text[],
	PROCEDURE = delete
);

CREATE OPERATOR - (
	LEFTARG = hstore,
	RIGHTARG = hstore,
	PROCEDURE = delete
);

CREATE FUNCTION hs_concat(hstore,hstore)
RETURNS hstore
AS 'MODULE_PATHNAME','hstore_concat'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR || (
	LEFTARG = hstore,
	RIGHTARG = hstore,
	PROCEDURE = hs_concat
);

CREATE FUNCTION hs_contains(hstore,hstore)
RETURNS bool
AS 'MODULE_PATHNAME','hstore_contains'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION hs_contained(hstore,hstore)
RETURNS bool
AS 'MODULE_PATHNAME','hstore_contained'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR @> (
	LEFTARG = hstore,
	RIGHTARG = hstore,
	PROCEDURE = hs_contains,
	COMMUTATOR = '<@',
	RESTRICT = contsel,
	JOIN = contjoinsel
);

CREATE OPERATOR <@ (
	LEFTARG = hstore,
	RIGHTARG = hstore,
	PROCEDURE = hs_contained,
	COMMUTATOR = '@>',
	RESTRICT = contsel,
	JOIN = contjoinsel
);

-- obsolete:
CREATE OPERATOR @ (
	LEFTARG = hstore,
	RIGHTARG = hstore,
	PROCEDURE = hs_contains,
	COMMUTATOR = '~',
	RESTRICT = contsel,
	JOIN = contjoinsel
);

CREATE OPERATOR ~ (
	LEFTARG = hstore,
	RIGHTARG = hstore,
	PROCEDURE = hs_contained,
	COMMUTATOR = '@',
	RESTRICT = contsel,
	JOIN = contjoinsel
);

CREATE FUNCTION tconvert(text,text)
RETURNS hstore
AS 'MODULE_PATHNAME','hstore_from_text'
LANGUAGE C IMMUTABLE; -- not STRICT; needs to allow (key,NULL)

CREATE FUNCTION hstore(text,text)
RETURNS hstore
AS 'MODULE_PATHNAME','hstore_from_text'
LANGUAGE C IMMUTABLE; -- not STRICT; needs to allow (key,NULL)

CREATE FUNCTION hstore(text[],text[])
RETURNS hstore
AS 'MODULE_PATHNAME', 'hstore_from_arrays'
LANGUAGE C IMMUTABLE; -- not STRICT; allows (keys,null)

CREATE FUNCTION hstore(text[])
RETURNS hstore
AS 'MODULE_PATHNAME', 'hstore_from_array'
LANGUAGE C IMMUTABLE STRICT;

CREATE CAST (text[] AS hstore)
  WITH FUNCTION hstore(text[]);

CREATE FUNCTION hstore(record)
RETURNS hstore
AS 'MODULE_PATHNAME', 'hstore_from_record'
LANGUAGE C IMMUTABLE; -- not STRICT; allows (null::recordtype)

CREATE FUNCTION hstore_to_array(hstore)
RETURNS text[]
AS 'MODULE_PATHNAME','hstore_to_array'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR %% (
       RIGHTARG = hstore,
       PROCEDURE = hstore_to_array
);

CREATE FUNCTION hstore_to_matrix(hstore)
RETURNS text[]
AS 'MODULE_PATHNAME','hstore_to_matrix'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR %# (
       RIGHTARG = hstore,
       PROCEDURE = hstore_to_matrix
);

CREATE FUNCTION akeys(hstore)
RETURNS text[]
AS 'MODULE_PATHNAME','hstore_akeys'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION avals(hstore)
RETURNS text[]
AS 'MODULE_PATHNAME','hstore_avals'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION skeys(hstore)
RETURNS setof text
AS 'MODULE_PATHNAME','hstore_skeys'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION svals(hstore)
RETURNS setof text
AS 'MODULE_PATHNAME','hstore_svals'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION each(IN hs hstore,
    OUT key text,
    OUT value text)
RETURNS SETOF record
AS 'MODULE_PATHNAME','hstore_each'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION populate_record(anyelement,hstore)
RETURNS anyelement
AS 'MODULE_PATHNAME', 'hstore_populate_record'
LANGUAGE C IMMUTABLE; -- not STRICT; allows (null::rectype,hstore)

CREATE OPERATOR #= (
	LEFTARG = anyelement,
	RIGHTARG = hstore,
	PROCEDURE = populate_record
);

-- btree support

CREATE FUNCTION hstore_eq(hstore,hstore)
RETURNS boolean
AS 'MODULE_PATHNAME','hstore_eq'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION hstore_ne(hstore,hstore)
RETURNS boolean
AS 'MODULE_PATHNAME','hstore_ne'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION hstore_gt(hstore,hstore)
RETURNS boolean
AS 'MODULE_PATHNAME','hstore_gt'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION hstore_ge(hstore,hstore)
RETURNS boolean
AS 'MODULE_PATHNAME','hstore_ge'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION hstore_lt(hstore,hstore)
RETURNS boolean
AS 'MODULE_PATHNAME','hstore_lt'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION hstore_le(hstore,hstore)
RETURNS boolean
AS 'MODULE_PATHNAME','hstore_le'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION hstore_cmp(hstore,hstore)
RETURNS integer
AS 'MODULE_PATHNAME','hstore_cmp'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR = (
       LEFTARG = hstore,
       RIGHTARG = hstore,
       PROCEDURE = hstore_eq,
       COMMUTATOR = =,
       NEGATOR = <>,
       RESTRICT = eqsel,
       JOIN = eqjoinsel,
       MERGES,
       HASHES
);
CREATE OPERATOR <> (
       LEFTARG = hstore,
       RIGHTARG = hstore,
       PROCEDURE = hstore_ne,
       COMMUTATOR = <>,
       NEGATOR = =,
       RESTRICT = neqsel,
       JOIN = neqjoinsel
);

-- the comparison operators have funky names (and are undocumented)
-- in an attempt to discourage anyone from actually using them. they
-- only exist to support the btree opclass

CREATE OPERATOR #<# (
       LEFTARG = hstore,
       RIGHTARG = hstore,
       PROCEDURE = hstore_lt,
       COMMUTATOR = #>#,
       NEGATOR = #>=#,
       RESTRICT = scalarltsel,
       JOIN = scalarltjoinsel
);
CREATE OPERATOR #<=# (
       LEFTARG = hstore,
       RIGHTARG = hstore,
       PROCEDURE = hstore_le,
       COMMUTATOR = #>=#,
       NEGATOR = #>#,
       RESTRICT = scalarltsel,
       JOIN = scalarltjoinsel
);
CREATE OPERATOR #># (
       LEFTARG = hstore,
       RIGHTARG = hstore,
       PROCEDURE = hstore_gt,
       COMMUTATOR = #<#,
       NEGATOR = #<=#,
       RESTRICT = scalargtsel,
       JOIN = scalargtjoinsel
);
CREATE OPERATOR #>=# (
       LEFTARG = hstore,
       RIGHTARG = hstore,
       PROCEDURE = hstore_ge,
       COMMUTATOR = #<=#,
       NEGATOR = #<#,
       RESTRICT = scalargtsel,
       JOIN = scalargtjoinsel
);

CREATE OPERATOR CLASS btree_hstore_ops
DEFAULT FOR TYPE hstore USING btree
AS
	OPERATOR	1	#<# ,
	OPERATOR	2	#<=# ,
	OPERATOR	3	= ,
	OPERATOR	4	#>=# ,
	OPERATOR	5	#># ,
	FUNCTION	1	hstore_cmp(hstore,hstore);

-- hash support

CREATE FUNCTION hstore_hash(hstore)
RETURNS integer
AS 'MODULE_PATHNAME','hstore_hash'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR CLASS hash_hstore_ops
DEFAULT FOR TYPE hstore USING hash
AS
	OPERATOR	1	= ,
	FUNCTION	1	hstore_hash(hstore);

-- GiST support

CREATE TYPE ghstore;

CREATE FUNCTION ghstore_in(cstring)
RETURNS ghstore
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION ghstore_out(ghstore)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE TYPE ghstore (
        INTERNALLENGTH = -1,
        INPUT = ghstore_in,
        OUTPUT = ghstore_out
);

CREATE FUNCTION ghstore_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION ghstore_decompress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION ghstore_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION ghstore_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION ghstore_union(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION ghstore_same(internal, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION ghstore_consistent(internal,internal,int,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR CLASS gist_hstore_ops
DEFAULT FOR TYPE hstore USING gist
AS
	OPERATOR        7       @> ,
	OPERATOR        9       ?(hstore,text) ,
	OPERATOR        10      ?|(hstore,text[]) ,
	OPERATOR        11      ?&(hstore,text[]) ,
        --OPERATOR        8       <@ ,
        OPERATOR        13      @ ,
        --OPERATOR        14      ~ ,
        FUNCTION        1       ghstore_consistent (internal, internal, int, oid, internal),
        FUNCTION        2       ghstore_union (internal, internal),
        FUNCTION        3       ghstore_compress (internal),
        FUNCTION        4       ghstore_decompress (internal),
        FUNCTION        5       ghstore_penalty (internal, internal, internal),
        FUNCTION        6       ghstore_picksplit (internal, internal),
        FUNCTION        7       ghstore_same (internal, internal, internal),
        STORAGE         ghstore;

-- GIN support

CREATE FUNCTION gin_extract_hstore(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gin_extract_hstore_query(internal, internal, int2, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gin_consistent_hstore(internal, int2, internal, int4, internal, internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR CLASS gin_hstore_ops
DEFAULT FOR TYPE hstore USING gin
AS
	OPERATOR        7       @>,
	OPERATOR        9       ?(hstore,text),
	OPERATOR        10      ?|(hstore,text[]),
	OPERATOR        11      ?&(hstore,text[]),
	FUNCTION        1       bttextcmp(text,text),
	FUNCTION        2       gin_extract_hstore(internal, internal),
	FUNCTION        3       gin_extract_hstore_query(internal, internal, int2, internal, internal),
	FUNCTION        4       gin_consistent_hstore(internal, int2, internal, int4, internal, internal),
	STORAGE         text;
