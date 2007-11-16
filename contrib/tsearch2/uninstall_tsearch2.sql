/* $PostgreSQL: pgsql/contrib/tsearch2/uninstall_tsearch2.sql,v 1.2 2007/11/16 00:34:54 tgl Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public, pg_catalog;

DROP DOMAIN tsvector CASCADE;
DROP DOMAIN tsquery CASCADE;
DROP DOMAIN gtsvector CASCADE;
DROP DOMAIN gtsq CASCADE;

DROP TYPE tokentype CASCADE;
DROP TYPE tokenout CASCADE;
DROP TYPE statinfo CASCADE;
DROP TYPE tsdebug CASCADE;

DROP OPERATOR CLASS tsquery_ops USING btree CASCADE;

DROP OPERATOR CLASS tsvector_ops USING btree CASCADE;

DROP OPERATOR CLASS gin_tsvector_ops USING gin CASCADE;

DROP OPERATOR CLASS gist_tp_tsquery_ops USING gist CASCADE;

DROP OPERATOR CLASS gist_tsvector_ops USING gist CASCADE;

DROP FUNCTION lexize(oid, text) ;
DROP FUNCTION lexize(text, text);
DROP FUNCTION lexize(text);
DROP FUNCTION set_curdict(int);
DROP FUNCTION set_curdict(text);
DROP FUNCTION dex_init(internal);
DROP FUNCTION dex_lexize(internal,internal,int4);
DROP FUNCTION snb_en_init(internal);
DROP FUNCTION snb_lexize(internal,internal,int4);
DROP FUNCTION snb_ru_init_koi8(internal);
DROP FUNCTION snb_ru_init_utf8(internal);
DROP FUNCTION snb_ru_init(internal);
DROP FUNCTION spell_init(internal);
DROP FUNCTION spell_lexize(internal,internal,int4);
DROP FUNCTION syn_init(internal);
DROP FUNCTION syn_lexize(internal,internal,int4);
DROP FUNCTION thesaurus_init(internal);
DROP FUNCTION thesaurus_lexize(internal,internal,int4,internal);
DROP FUNCTION set_curprs(int);
DROP FUNCTION set_curprs(text);
DROP FUNCTION prsd_start(internal,int4);
DROP FUNCTION prsd_getlexeme(internal,internal,internal);
DROP FUNCTION prsd_end(internal);
DROP FUNCTION prsd_lextype(internal);
DROP FUNCTION prsd_headline(internal,internal,internal);
DROP FUNCTION set_curcfg(int);
DROP FUNCTION set_curcfg(text);
DROP FUNCTION show_curcfg();
DROP FUNCTION length(tsvector);
DROP FUNCTION to_tsvector(oid, text);
DROP FUNCTION to_tsvector(text, text);
DROP FUNCTION to_tsvector(text);
DROP FUNCTION strip(tsvector);
DROP FUNCTION setweight(tsvector,"char");
DROP FUNCTION concat(tsvector,tsvector);
DROP FUNCTION querytree(tsquery);
DROP FUNCTION to_tsquery(oid, text);
DROP FUNCTION to_tsquery(text, text);
DROP FUNCTION to_tsquery(text);
DROP FUNCTION plainto_tsquery(oid, text);
DROP FUNCTION plainto_tsquery(text, text);
DROP FUNCTION plainto_tsquery(text);
DROP FUNCTION tsearch2() CASCADE;
DROP FUNCTION rank(float4[], tsvector, tsquery);
DROP FUNCTION rank(float4[], tsvector, tsquery, int4);
DROP FUNCTION rank(tsvector, tsquery);
DROP FUNCTION rank(tsvector, tsquery, int4);
DROP FUNCTION rank_cd(float4[], tsvector, tsquery);
DROP FUNCTION rank_cd(float4[], tsvector, tsquery, int4);
DROP FUNCTION rank_cd(tsvector, tsquery);
DROP FUNCTION rank_cd(tsvector, tsquery, int4);
DROP FUNCTION headline(oid, text, tsquery, text);
DROP FUNCTION headline(oid, text, tsquery);
DROP FUNCTION headline(text, text, tsquery, text);
DROP FUNCTION headline(text, text, tsquery);
DROP FUNCTION headline(text, tsquery, text);
DROP FUNCTION headline(text, tsquery);
DROP FUNCTION get_covers(tsvector,tsquery);
DROP FUNCTION _get_parser_from_curcfg();
DROP FUNCTION numnode(tsquery);
DROP FUNCTION tsquery_and(tsquery,tsquery);
DROP FUNCTION tsquery_or(tsquery,tsquery);
DROP FUNCTION tsquery_not(tsquery);
DROP FUNCTION rewrite(tsquery, text);
DROP FUNCTION rewrite(tsquery, tsquery, tsquery);
DROP AGGREGATE rewrite (tsquery[]);
DROP FUNCTION rewrite_accum(tsquery,tsquery[]);
DROP FUNCTION rewrite_finish(tsquery);
DROP FUNCTION tsq_mcontains(tsquery, tsquery);
DROP FUNCTION tsq_mcontained(tsquery, tsquery);
DROP FUNCTION reset_tsearch();
