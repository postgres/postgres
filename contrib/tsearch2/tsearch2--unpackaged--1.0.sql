/* contrib/tsearch2/tsearch2--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION tsearch2" to load this file. \quit

ALTER EXTENSION tsearch2 ADD type @extschema@.tsvector;
ALTER EXTENSION tsearch2 ADD type @extschema@.tsquery;
ALTER EXTENSION tsearch2 ADD type @extschema@.gtsvector;
ALTER EXTENSION tsearch2 ADD type gtsq;
ALTER EXTENSION tsearch2 ADD function lexize(oid,text);
ALTER EXTENSION tsearch2 ADD function lexize(text,text);
ALTER EXTENSION tsearch2 ADD function lexize(text);
ALTER EXTENSION tsearch2 ADD function set_curdict(integer);
ALTER EXTENSION tsearch2 ADD function set_curdict(text);
ALTER EXTENSION tsearch2 ADD function dex_init(internal);
ALTER EXTENSION tsearch2 ADD function dex_lexize(internal,internal,integer);
ALTER EXTENSION tsearch2 ADD function snb_en_init(internal);
ALTER EXTENSION tsearch2 ADD function snb_lexize(internal,internal,integer);
ALTER EXTENSION tsearch2 ADD function snb_ru_init_koi8(internal);
ALTER EXTENSION tsearch2 ADD function snb_ru_init_utf8(internal);
ALTER EXTENSION tsearch2 ADD function snb_ru_init(internal);
ALTER EXTENSION tsearch2 ADD function spell_init(internal);
ALTER EXTENSION tsearch2 ADD function spell_lexize(internal,internal,integer);
ALTER EXTENSION tsearch2 ADD function syn_init(internal);
ALTER EXTENSION tsearch2 ADD function syn_lexize(internal,internal,integer);
ALTER EXTENSION tsearch2 ADD function @extschema@.thesaurus_init(internal);
ALTER EXTENSION tsearch2 ADD function thesaurus_lexize(internal,internal,integer,internal);
ALTER EXTENSION tsearch2 ADD type tokentype;
ALTER EXTENSION tsearch2 ADD function token_type(integer);
ALTER EXTENSION tsearch2 ADD function token_type(text);
ALTER EXTENSION tsearch2 ADD function token_type();
ALTER EXTENSION tsearch2 ADD function set_curprs(integer);
ALTER EXTENSION tsearch2 ADD function set_curprs(text);
ALTER EXTENSION tsearch2 ADD type tokenout;
ALTER EXTENSION tsearch2 ADD function parse(oid,text);
ALTER EXTENSION tsearch2 ADD function parse(text,text);
ALTER EXTENSION tsearch2 ADD function parse(text);
ALTER EXTENSION tsearch2 ADD function @extschema@.prsd_start(internal,integer);
ALTER EXTENSION tsearch2 ADD function prsd_getlexeme(internal,internal,internal);
ALTER EXTENSION tsearch2 ADD function @extschema@.prsd_end(internal);
ALTER EXTENSION tsearch2 ADD function @extschema@.prsd_lextype(internal);
ALTER EXTENSION tsearch2 ADD function prsd_headline(internal,internal,internal);
ALTER EXTENSION tsearch2 ADD function set_curcfg(integer);
ALTER EXTENSION tsearch2 ADD function set_curcfg(text);
ALTER EXTENSION tsearch2 ADD function show_curcfg();
ALTER EXTENSION tsearch2 ADD function @extschema@.length(tsvector);
ALTER EXTENSION tsearch2 ADD function to_tsvector(oid,text);
ALTER EXTENSION tsearch2 ADD function to_tsvector(text,text);
ALTER EXTENSION tsearch2 ADD function @extschema@.to_tsvector(text);
ALTER EXTENSION tsearch2 ADD function @extschema@.strip(tsvector);
ALTER EXTENSION tsearch2 ADD function @extschema@.setweight(tsvector,"char");
ALTER EXTENSION tsearch2 ADD function concat(tsvector,tsvector);
ALTER EXTENSION tsearch2 ADD function @extschema@.querytree(tsquery);
ALTER EXTENSION tsearch2 ADD function to_tsquery(oid,text);
ALTER EXTENSION tsearch2 ADD function to_tsquery(text,text);
ALTER EXTENSION tsearch2 ADD function @extschema@.to_tsquery(text);
ALTER EXTENSION tsearch2 ADD function plainto_tsquery(oid,text);
ALTER EXTENSION tsearch2 ADD function plainto_tsquery(text,text);
ALTER EXTENSION tsearch2 ADD function @extschema@.plainto_tsquery(text);
ALTER EXTENSION tsearch2 ADD function tsearch2();
ALTER EXTENSION tsearch2 ADD function rank(real[],tsvector,tsquery);
ALTER EXTENSION tsearch2 ADD function rank(real[],tsvector,tsquery,integer);
ALTER EXTENSION tsearch2 ADD function rank(tsvector,tsquery);
ALTER EXTENSION tsearch2 ADD function rank(tsvector,tsquery,integer);
ALTER EXTENSION tsearch2 ADD function rank_cd(real[],tsvector,tsquery);
ALTER EXTENSION tsearch2 ADD function rank_cd(real[],tsvector,tsquery,integer);
ALTER EXTENSION tsearch2 ADD function rank_cd(tsvector,tsquery);
ALTER EXTENSION tsearch2 ADD function rank_cd(tsvector,tsquery,integer);
ALTER EXTENSION tsearch2 ADD function headline(oid,text,tsquery,text);
ALTER EXTENSION tsearch2 ADD function headline(oid,text,tsquery);
ALTER EXTENSION tsearch2 ADD function headline(text,text,tsquery,text);
ALTER EXTENSION tsearch2 ADD function headline(text,text,tsquery);
ALTER EXTENSION tsearch2 ADD function headline(text,tsquery,text);
ALTER EXTENSION tsearch2 ADD function headline(text,tsquery);
ALTER EXTENSION tsearch2 ADD operator family gist_tsvector_ops using gist;
ALTER EXTENSION tsearch2 ADD operator class gist_tsvector_ops using gist;
ALTER EXTENSION tsearch2 ADD type statinfo;
ALTER EXTENSION tsearch2 ADD function stat(text);
ALTER EXTENSION tsearch2 ADD function stat(text,text);
ALTER EXTENSION tsearch2 ADD function reset_tsearch();
ALTER EXTENSION tsearch2 ADD function get_covers(tsvector,tsquery);
ALTER EXTENSION tsearch2 ADD type tsdebug;
ALTER EXTENSION tsearch2 ADD function _get_parser_from_curcfg();
ALTER EXTENSION tsearch2 ADD function @extschema@.ts_debug(text);
ALTER EXTENSION tsearch2 ADD function @extschema@.numnode(tsquery);
ALTER EXTENSION tsearch2 ADD function @extschema@.tsquery_and(tsquery,tsquery);
ALTER EXTENSION tsearch2 ADD function @extschema@.tsquery_or(tsquery,tsquery);
ALTER EXTENSION tsearch2 ADD function @extschema@.tsquery_not(tsquery);
ALTER EXTENSION tsearch2 ADD function rewrite(tsquery,text);
ALTER EXTENSION tsearch2 ADD function rewrite(tsquery,tsquery,tsquery);
ALTER EXTENSION tsearch2 ADD function rewrite_accum(tsquery,tsquery[]);
ALTER EXTENSION tsearch2 ADD function rewrite_finish(tsquery);
ALTER EXTENSION tsearch2 ADD function rewrite(tsquery[]);
ALTER EXTENSION tsearch2 ADD function @extschema@.tsq_mcontains(tsquery,tsquery);
ALTER EXTENSION tsearch2 ADD function @extschema@.tsq_mcontained(tsquery,tsquery);
ALTER EXTENSION tsearch2 ADD operator family gist_tp_tsquery_ops using gist;
ALTER EXTENSION tsearch2 ADD operator class gist_tp_tsquery_ops using gist;
ALTER EXTENSION tsearch2 ADD operator family gin_tsvector_ops using gin;
ALTER EXTENSION tsearch2 ADD operator class gin_tsvector_ops using gin;
ALTER EXTENSION tsearch2 ADD operator family @extschema@.tsvector_ops using btree;
ALTER EXTENSION tsearch2 ADD operator class @extschema@.tsvector_ops using btree;
ALTER EXTENSION tsearch2 ADD operator family @extschema@.tsquery_ops using btree;
ALTER EXTENSION tsearch2 ADD operator class @extschema@.tsquery_ops using btree;

-- tsearch2 relies on the core functions gin_extract_tsvector,
-- gin_extract_tsquery, and gin_tsquery_consistent, which changed signature in
-- 9.1.  To support upgrading, pg_catalog contains entries for these functions
-- with both the old and new signatures, and the former is what would have
-- been added to our opclass during initial restore of a 9.0 dump script.
-- Avert your eyes while we hack the pg_amproc entries to make them link to
-- the new forms ...

UPDATE pg_catalog.pg_amproc
SET amproc = 'pg_catalog.gin_extract_tsvector(pg_catalog.tsvector,internal,internal)'::pg_catalog.regprocedure
WHERE amprocfamily =
  (SELECT oid FROM pg_catalog.pg_opfamily WHERE opfname = 'gin_tsvector_ops' AND
     opfnamespace = (SELECT oid FROM pg_catalog.pg_namespace
                     WHERE nspname = '@extschema@'))
  AND amproclefttype = 'pg_catalog.tsvector'::pg_catalog.regtype
  AND amprocrighttype = 'pg_catalog.tsvector'::pg_catalog.regtype
  AND amprocnum = 2
  AND amproc = 'pg_catalog.gin_extract_tsvector(pg_catalog.tsvector,internal)'::pg_catalog.regprocedure;

UPDATE pg_catalog.pg_amproc
SET amproc = 'pg_catalog.gin_extract_tsquery(pg_catalog.tsquery,internal,smallint,internal,internal,internal,internal)'::pg_catalog.regprocedure
WHERE amprocfamily =
  (SELECT oid FROM pg_catalog.pg_opfamily WHERE opfname = 'gin_tsvector_ops' AND
     opfnamespace = (SELECT oid FROM pg_catalog.pg_namespace
                     WHERE nspname = '@extschema@'))
  AND amproclefttype = 'pg_catalog.tsvector'::pg_catalog.regtype
  AND amprocrighttype = 'pg_catalog.tsvector'::pg_catalog.regtype
  AND amprocnum = 3
  AND amproc = 'pg_catalog.gin_extract_tsquery(pg_catalog.tsquery,internal,smallint,internal,internal)'::pg_catalog.regprocedure;

UPDATE pg_catalog.pg_amproc
SET amproc = 'pg_catalog.gin_tsquery_consistent(internal,smallint,pg_catalog.tsquery,integer,internal,internal,internal,internal)'::pg_catalog.regprocedure
WHERE amprocfamily =
  (SELECT oid FROM pg_catalog.pg_opfamily WHERE opfname = 'gin_tsvector_ops' AND
     opfnamespace = (SELECT oid FROM pg_catalog.pg_namespace
                     WHERE nspname = '@extschema@'))
  AND amproclefttype = 'pg_catalog.tsvector'::pg_catalog.regtype
  AND amprocrighttype = 'pg_catalog.tsvector'::pg_catalog.regtype
  AND amprocnum = 4
  AND amproc = 'pg_catalog.gin_tsquery_consistent(internal,smallint,pg_catalog.tsquery,integer,internal,internal)'::pg_catalog.regprocedure;
