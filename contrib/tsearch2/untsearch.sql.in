BEGIN;

--Be careful !!!
--script drops all indices, triggers and columns with types defined
--in tsearch2.sql


DROP OPERATOR CLASS gin_tsvector_ops USING gin CASCADE;

DROP OPERATOR CLASS gist_tsvector_ops USING gist CASCADE;


DROP OPERATOR || (tsvector, tsvector);
DROP OPERATOR @@ (tsvector, tsquery);
DROP OPERATOR @@ (tsquery, tsvector);

--DROP AGGREGATE stat(tsvector);

DROP TABLE pg_ts_dict;
DROP TABLE pg_ts_parser;
DROP TABLE pg_ts_cfg;
DROP TABLE pg_ts_cfgmap;

DROP TYPE tokentype CASCADE;
DROP TYPE tokenout CASCADE;
DROP TYPE tsvector CASCADE;
DROP TYPE tsquery CASCADE;
DROP TYPE gtsvector CASCADE;
--DROP TYPE tsstat CASCADE;
DROP TYPE statinfo CASCADE;
DROP TYPE tsdebug CASCADE;
DROP TYPE gtsq CASCADE;

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
DROP FUNCTION spell_init(internal);
DROP FUNCTION spell_lexize(internal,internal,int4);
DROP FUNCTION thesaurus_init(internal);
DROP FUNCTION thesaurus_lexize(internal,internal,int4,internal);
DROP FUNCTION syn_init(internal);
DROP FUNCTION syn_lexize(internal,internal,int4);
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
DROP FUNCTION gtsvector_compress(internal);
DROP FUNCTION gtsvector_decompress(internal);
DROP FUNCTION gtsvector_penalty(internal,internal,internal);
DROP FUNCTION gtsvector_picksplit(internal, internal);
DROP FUNCTION gtsvector_union(internal, internal);
DROP FUNCTION gtsq_compress(internal);
DROP FUNCTION gtsq_decompress(internal);
DROP FUNCTION gtsq_penalty(internal,internal,internal);
DROP FUNCTION gtsq_picksplit(internal, internal);
DROP FUNCTION gtsq_union(bytea, internal);
DROP FUNCTION reset_tsearch();
DROP FUNCTION tsearch2() CASCADE;
DROP FUNCTION _get_parser_from_curcfg();

END;
