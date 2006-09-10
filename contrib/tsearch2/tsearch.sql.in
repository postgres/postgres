-- Adjust this setting to control where the objects get CREATEd.
SET search_path = public;

BEGIN;

--dict conf
CREATE TABLE pg_ts_dict (
	dict_name	text not null primary key,
	dict_init	regprocedure,
	dict_initoption	text,
	dict_lexize	regprocedure not null,
	dict_comment	text
) with oids;

--dict interface
CREATE FUNCTION lexize(oid, text) 
	RETURNS _text
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

CREATE FUNCTION lexize(text, text)
        RETURNS _text
        as 'MODULE_PATHNAME', 'lexize_byname'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT;

CREATE FUNCTION lexize(text)
        RETURNS _text
        as 'MODULE_PATHNAME', 'lexize_bycurrent'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT;

CREATE FUNCTION set_curdict(int)
	RETURNS void
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

CREATE FUNCTION set_curdict(text)
	RETURNS void
	as 'MODULE_PATHNAME', 'set_curdict_byname'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

--built-in dictionaries
CREATE FUNCTION dex_init(internal)
	RETURNS internal
	as 'MODULE_PATHNAME' 
	LANGUAGE C;

CREATE FUNCTION dex_lexize(internal,internal,int4)
	RETURNS internal
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

insert into pg_ts_dict select 
	'simple', 
	'dex_init(internal)',
	null,
	'dex_lexize(internal,internal,int4)',
	'Simple example of dictionary.'
;
	 
CREATE FUNCTION snb_en_init(internal)
	RETURNS internal
	as 'MODULE_PATHNAME' 
	LANGUAGE C;

CREATE FUNCTION snb_lexize(internal,internal,int4)
	RETURNS internal
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

insert into pg_ts_dict select 
	'en_stem', 
	'snb_en_init(internal)',
	'contrib/english.stop',
	'snb_lexize(internal,internal,int4)',
	'English Stemmer. Snowball.'
;

CREATE FUNCTION snb_ru_init_koi8(internal)
	RETURNS internal
	as 'MODULE_PATHNAME' 
	LANGUAGE C;

insert into pg_ts_dict select 
	'ru_stem_koi8', 
	'snb_ru_init_koi8(internal)',
	'contrib/russian.stop',
	'snb_lexize(internal,internal,int4)',
	'Russian Stemmer. Snowball. KOI8 Encoding'
;
	 
CREATE FUNCTION snb_ru_init_utf8(internal)
	RETURNS internal
	as 'MODULE_PATHNAME' 
	LANGUAGE C;

insert into pg_ts_dict select 
	'ru_stem_utf8', 
	'snb_ru_init_utf8(internal)',
	'contrib/russian.stop.utf8',
	'snb_lexize(internal,internal,int4)',
	'Russian Stemmer. Snowball. UTF8 Encoding'
;
	 
CREATE FUNCTION spell_init(internal)
	RETURNS internal
	as 'MODULE_PATHNAME' 
	LANGUAGE C;

CREATE FUNCTION spell_lexize(internal,internal,int4)
	RETURNS internal
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

insert into pg_ts_dict select 
	'ispell_template', 
	'spell_init(internal)',
	null,
	'spell_lexize(internal,internal,int4)',
	'ISpell interface. Must have .dict and .aff files'
;

CREATE FUNCTION syn_init(internal)
	RETURNS internal
	as 'MODULE_PATHNAME' 
	LANGUAGE C;

CREATE FUNCTION syn_lexize(internal,internal,int4)
	RETURNS internal
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

insert into pg_ts_dict select 
	'synonym', 
	'syn_init(internal)',
	null,
	'syn_lexize(internal,internal,int4)',
	'Example of synonym dictionary'
;

CREATE FUNCTION thesaurus_init(internal)
	RETURNS internal
	as 'MODULE_PATHNAME' 
	LANGUAGE C;

CREATE FUNCTION thesaurus_lexize(internal,internal,int4,internal)
	RETURNS internal
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

insert into pg_ts_dict select 
	'thesaurus_template', 
	'thesaurus_init(internal)',
	null,
	'thesaurus_lexize(internal,internal,int4,internal)',
	'Thesaurus template, must be pointed Dictionary and DictFile'
;

--dict conf
CREATE TABLE pg_ts_parser (
	prs_name	text not null primary key,
	prs_start	regprocedure not null,
	prs_nexttoken	regprocedure not null,
	prs_end		regprocedure not null,
	prs_headline	regprocedure not null,
	prs_lextype	regprocedure not null,
	prs_comment	text
) with oids;

--sql-level interface
CREATE TYPE tokentype 
	as (tokid int4, alias text, descr text); 

CREATE FUNCTION token_type(int4)
	RETURNS setof tokentype
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

CREATE FUNCTION token_type(text)
	RETURNS setof tokentype
	as 'MODULE_PATHNAME', 'token_type_byname'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

CREATE FUNCTION token_type()
	RETURNS setof tokentype
	as 'MODULE_PATHNAME', 'token_type_current'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

CREATE FUNCTION set_curprs(int)
	RETURNS void
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

CREATE FUNCTION set_curprs(text)
	RETURNS void
	as 'MODULE_PATHNAME', 'set_curprs_byname'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

CREATE TYPE tokenout 
	as (tokid int4, token text);

CREATE FUNCTION parse(oid,text)
	RETURNS setof tokenout
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;
 
CREATE FUNCTION parse(text,text)
	RETURNS setof tokenout
	as 'MODULE_PATHNAME', 'parse_byname'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;
 
CREATE FUNCTION parse(text)
	RETURNS setof tokenout
	as 'MODULE_PATHNAME', 'parse_current'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;
 
--default parser
CREATE FUNCTION prsd_start(internal,int4)
	RETURNS internal
	as 'MODULE_PATHNAME'
	LANGUAGE C;

CREATE FUNCTION prsd_getlexeme(internal,internal,internal)
	RETURNS int4
	as 'MODULE_PATHNAME'
	LANGUAGE C;

CREATE FUNCTION prsd_end(internal)
	RETURNS void
	as 'MODULE_PATHNAME'
	LANGUAGE C;

CREATE FUNCTION prsd_lextype(internal)
	RETURNS internal
	as 'MODULE_PATHNAME'
	LANGUAGE C;

CREATE FUNCTION prsd_headline(internal,internal,internal)
	RETURNS internal
	as 'MODULE_PATHNAME'
	LANGUAGE C;

insert into pg_ts_parser select
	'default',
	'prsd_start(internal,int4)',
	'prsd_getlexeme(internal,internal,internal)',
	'prsd_end(internal)',
	'prsd_headline(internal,internal,internal)',
	'prsd_lextype(internal)',
	'Parser from OpenFTS v0.34'
;	

--tsearch config

CREATE TABLE pg_ts_cfg (
	ts_name		text not null primary key,
	prs_name	text not null,
	locale		text
) with oids;

CREATE TABLE pg_ts_cfgmap (
	ts_name		text not null,
	tok_alias	text not null,
	dict_name	text[],
	primary key (ts_name,tok_alias)
) with oids;

CREATE FUNCTION set_curcfg(int)
	RETURNS void
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

CREATE FUNCTION set_curcfg(text)
	RETURNS void
	as 'MODULE_PATHNAME', 'set_curcfg_byname'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

CREATE FUNCTION show_curcfg()
	RETURNS oid
	as 'MODULE_PATHNAME'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

insert into pg_ts_cfg values ('default', 'default','C');
insert into pg_ts_cfg values ('default_russian', 'default','ru_RU.KOI8-R');
insert into pg_ts_cfg values ('utf8_russian', 'default','ru_RU.UTF-8');
insert into pg_ts_cfg values ('simple', 'default');

insert into pg_ts_cfgmap values ('default', 'lword', '{en_stem}');
insert into pg_ts_cfgmap values ('default', 'nlword', '{simple}');
insert into pg_ts_cfgmap values ('default', 'word', '{simple}');
insert into pg_ts_cfgmap values ('default', 'email', '{simple}');
insert into pg_ts_cfgmap values ('default', 'url', '{simple}');
insert into pg_ts_cfgmap values ('default', 'host', '{simple}');
insert into pg_ts_cfgmap values ('default', 'sfloat', '{simple}');
insert into pg_ts_cfgmap values ('default', 'version', '{simple}');
insert into pg_ts_cfgmap values ('default', 'part_hword', '{simple}');
insert into pg_ts_cfgmap values ('default', 'nlpart_hword', '{simple}');
insert into pg_ts_cfgmap values ('default', 'lpart_hword', '{en_stem}');
insert into pg_ts_cfgmap values ('default', 'hword', '{simple}');
insert into pg_ts_cfgmap values ('default', 'lhword', '{en_stem}');
insert into pg_ts_cfgmap values ('default', 'nlhword', '{simple}');
insert into pg_ts_cfgmap values ('default', 'uri', '{simple}');
insert into pg_ts_cfgmap values ('default', 'file', '{simple}');
insert into pg_ts_cfgmap values ('default', 'float', '{simple}');
insert into pg_ts_cfgmap values ('default', 'int', '{simple}');
insert into pg_ts_cfgmap values ('default', 'uint', '{simple}');
insert into pg_ts_cfgmap values ('default_russian', 'lword', '{en_stem}');
insert into pg_ts_cfgmap values ('default_russian', 'nlword', '{ru_stem_koi8}');
insert into pg_ts_cfgmap values ('default_russian', 'word', '{ru_stem_koi8}');
insert into pg_ts_cfgmap values ('default_russian', 'email', '{simple}');
insert into pg_ts_cfgmap values ('default_russian', 'url', '{simple}');
insert into pg_ts_cfgmap values ('default_russian', 'host', '{simple}');
insert into pg_ts_cfgmap values ('default_russian', 'sfloat', '{simple}');
insert into pg_ts_cfgmap values ('default_russian', 'version', '{simple}');
insert into pg_ts_cfgmap values ('default_russian', 'part_hword', '{simple}');
insert into pg_ts_cfgmap values ('default_russian', 'nlpart_hword', '{ru_stem_koi8}');
insert into pg_ts_cfgmap values ('default_russian', 'lpart_hword', '{en_stem}');
insert into pg_ts_cfgmap values ('default_russian', 'hword', '{ru_stem_koi8}');
insert into pg_ts_cfgmap values ('default_russian', 'lhword', '{en_stem}');
insert into pg_ts_cfgmap values ('default_russian', 'nlhword', '{ru_stem_koi8}');
insert into pg_ts_cfgmap values ('default_russian', 'uri', '{simple}');
insert into pg_ts_cfgmap values ('default_russian', 'file', '{simple}');
insert into pg_ts_cfgmap values ('default_russian', 'float', '{simple}');
insert into pg_ts_cfgmap values ('default_russian', 'int', '{simple}');
insert into pg_ts_cfgmap values ('default_russian', 'uint', '{simple}');
insert into pg_ts_cfgmap values ('utf8_russian', 'lword', '{en_stem}');
insert into pg_ts_cfgmap values ('utf8_russian', 'nlword', '{ru_stem_utf8}');
insert into pg_ts_cfgmap values ('utf8_russian', 'word', '{ru_stem_utf8}');
insert into pg_ts_cfgmap values ('utf8_russian', 'email', '{simple}');
insert into pg_ts_cfgmap values ('utf8_russian', 'url', '{simple}');
insert into pg_ts_cfgmap values ('utf8_russian', 'host', '{simple}');
insert into pg_ts_cfgmap values ('utf8_russian', 'sfloat', '{simple}');
insert into pg_ts_cfgmap values ('utf8_russian', 'version', '{simple}');
insert into pg_ts_cfgmap values ('utf8_russian', 'part_hword', '{simple}');
insert into pg_ts_cfgmap values ('utf8_russian', 'nlpart_hword', '{ru_stem_utf8}');
insert into pg_ts_cfgmap values ('utf8_russian', 'lpart_hword', '{en_stem}');
insert into pg_ts_cfgmap values ('utf8_russian', 'hword', '{ru_stem_utf8}');
insert into pg_ts_cfgmap values ('utf8_russian', 'lhword', '{en_stem}');
insert into pg_ts_cfgmap values ('utf8_russian', 'nlhword', '{ru_stem_utf8}');
insert into pg_ts_cfgmap values ('utf8_russian', 'uri', '{simple}');
insert into pg_ts_cfgmap values ('utf8_russian', 'file', '{simple}');
insert into pg_ts_cfgmap values ('utf8_russian', 'float', '{simple}');
insert into pg_ts_cfgmap values ('utf8_russian', 'int', '{simple}');
insert into pg_ts_cfgmap values ('utf8_russian', 'uint', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'lword', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'nlword', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'word', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'email', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'url', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'host', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'sfloat', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'version', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'part_hword', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'nlpart_hword', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'lpart_hword', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'hword', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'lhword', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'nlhword', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'uri', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'file', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'float', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'int', '{simple}');
insert into pg_ts_cfgmap values ('simple', 'uint', '{simple}');

--tsvector type
CREATE FUNCTION tsvector_in(cstring)
RETURNS tsvector
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE FUNCTION tsvector_out(tsvector)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE TYPE tsvector (
        INTERNALLENGTH = -1,
        INPUT = tsvector_in,
        OUTPUT = tsvector_out,
        STORAGE = extended
);

CREATE FUNCTION length(tsvector)
RETURNS int4
AS 'MODULE_PATHNAME', 'tsvector_length'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION to_tsvector(oid, text)
RETURNS tsvector
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION to_tsvector(text, text)
RETURNS tsvector
AS 'MODULE_PATHNAME', 'to_tsvector_name'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION to_tsvector(text)
RETURNS tsvector
AS 'MODULE_PATHNAME', 'to_tsvector_current'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION strip(tsvector)
RETURNS tsvector
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION setweight(tsvector,"char")
RETURNS tsvector
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION concat(tsvector,tsvector)
RETURNS tsvector
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OPERATOR || (
        LEFTARG = tsvector,
        RIGHTARG = tsvector,
        PROCEDURE = concat
);

--query type
CREATE FUNCTION tsquery_in(cstring)
RETURNS tsquery
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE FUNCTION tsquery_out(tsquery)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE TYPE tsquery (
        INTERNALLENGTH = -1,
        ALIGNMENT = int4,
        INPUT = tsquery_in,
        OUTPUT = tsquery_out
);

CREATE FUNCTION querytree(tsquery)
RETURNS text
AS 'MODULE_PATHNAME', 'tsquerytree'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE FUNCTION to_tsquery(oid, text)
RETURNS tsquery
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION to_tsquery(text, text)
RETURNS tsquery
AS 'MODULE_PATHNAME','to_tsquery_name'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION to_tsquery(text)
RETURNS tsquery
AS 'MODULE_PATHNAME','to_tsquery_current'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION plainto_tsquery(oid, text)
RETURNS tsquery
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION plainto_tsquery(text, text)
RETURNS tsquery
AS 'MODULE_PATHNAME','plainto_tsquery_name'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION plainto_tsquery(text)
RETURNS tsquery
AS 'MODULE_PATHNAME','plainto_tsquery_current'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

--operations
CREATE FUNCTION exectsq(tsvector, tsquery)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;
  
COMMENT ON FUNCTION exectsq(tsvector, tsquery) IS 'boolean operation with text index';

CREATE FUNCTION rexectsq(tsquery, tsvector)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

COMMENT ON FUNCTION rexectsq(tsquery, tsvector) IS 'boolean operation with text index';

CREATE OPERATOR @@ (
        LEFTARG = tsvector,
        RIGHTARG = tsquery,
        PROCEDURE = exectsq,
        COMMUTATOR = '@@',
        RESTRICT = contsel,
        JOIN = contjoinsel
);
CREATE OPERATOR @@ (
        LEFTARG = tsquery,
        RIGHTARG = tsvector,
        PROCEDURE = rexectsq,
        COMMUTATOR = '@@',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

--Trigger
CREATE FUNCTION tsearch2()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

--Relevation
CREATE FUNCTION rank(float4[], tsvector, tsquery)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION rank(float4[], tsvector, tsquery, int4)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION rank(tsvector, tsquery)
RETURNS float4
AS 'MODULE_PATHNAME', 'rank_def'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION rank(tsvector, tsquery, int4)
RETURNS float4
AS 'MODULE_PATHNAME', 'rank_def'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION rank_cd(float4[], tsvector, tsquery)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION rank_cd(float4[], tsvector, tsquery, int4)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION rank_cd(tsvector, tsquery)
RETURNS float4
AS 'MODULE_PATHNAME', 'rank_cd_def'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION rank_cd(tsvector, tsquery, int4)
RETURNS float4
AS 'MODULE_PATHNAME', 'rank_cd_def'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION headline(oid, text, tsquery, text)
RETURNS text
AS 'MODULE_PATHNAME', 'headline'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION headline(oid, text, tsquery)
RETURNS text
AS 'MODULE_PATHNAME', 'headline'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION headline(text, text, tsquery, text)
RETURNS text
AS 'MODULE_PATHNAME', 'headline_byname'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION headline(text, text, tsquery)
RETURNS text
AS 'MODULE_PATHNAME', 'headline_byname'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION headline(text, tsquery, text)
RETURNS text
AS 'MODULE_PATHNAME', 'headline_current'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION headline(text, tsquery)
RETURNS text
AS 'MODULE_PATHNAME', 'headline_current'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

--GiST
--GiST key type 
CREATE FUNCTION gtsvector_in(cstring)
RETURNS gtsvector
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE FUNCTION gtsvector_out(gtsvector)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE TYPE gtsvector (
        INTERNALLENGTH = -1,
        INPUT = gtsvector_in,
        OUTPUT = gtsvector_out
);

-- support FUNCTIONs
CREATE FUNCTION gtsvector_consistent(gtsvector,internal,int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
  
CREATE FUNCTION gtsvector_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION gtsvector_decompress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION gtsvector_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE FUNCTION gtsvector_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION gtsvector_union(internal, internal)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION gtsvector_same(gtsvector, gtsvector, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- CREATE the OPERATOR class
CREATE OPERATOR CLASS gist_tsvector_ops
DEFAULT FOR TYPE tsvector USING gist
AS
        OPERATOR        1       @@ (tsvector, tsquery)  RECHECK ,
        FUNCTION        1       gtsvector_consistent (gtsvector, internal, int4),
        FUNCTION        2       gtsvector_union (internal, internal),
        FUNCTION        3       gtsvector_compress (internal),
        FUNCTION        4       gtsvector_decompress (internal),
        FUNCTION        5       gtsvector_penalty (internal, internal, internal),
        FUNCTION        6       gtsvector_picksplit (internal, internal),
        FUNCTION        7       gtsvector_same (gtsvector, gtsvector, internal),
        STORAGE         gtsvector;


--stat info
CREATE TYPE statinfo 
	as (word text, ndoc int4, nentry int4);

--CREATE FUNCTION tsstat_in(cstring)
--RETURNS tsstat
--AS 'MODULE_PATHNAME'
--LANGUAGE C RETURNS NULL ON NULL INPUT;
--
--CREATE FUNCTION tsstat_out(tsstat)
--RETURNS cstring
--AS 'MODULE_PATHNAME'
--LANGUAGE C RETURNS NULL ON NULL INPUT;
--
--CREATE TYPE tsstat (
--        INTERNALLENGTH = -1,
--        INPUT = tsstat_in,
--        OUTPUT = tsstat_out,
--        STORAGE = plain
--);
--
--CREATE FUNCTION ts_accum(tsstat,tsvector)
--RETURNS tsstat
--AS 'MODULE_PATHNAME'
--LANGUAGE C RETURNS NULL ON NULL INPUT;
--
--CREATE FUNCTION ts_accum_finish(tsstat)
--	RETURNS setof statinfo
--	as 'MODULE_PATHNAME'
--	LANGUAGE C
--	RETURNS NULL ON NULL INPUT;
--
--CREATE AGGREGATE stat (
--	BASETYPE=tsvector,
--	SFUNC=ts_accum,
--	STYPE=tsstat,
--	FINALFUNC = ts_accum_finish,
--	initcond = ''
--); 

CREATE FUNCTION stat(text)
	RETURNS setof statinfo
	as 'MODULE_PATHNAME', 'ts_stat'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

CREATE FUNCTION stat(text,text)
	RETURNS setof statinfo
	as 'MODULE_PATHNAME', 'ts_stat'
	LANGUAGE C
	RETURNS NULL ON NULL INPUT;

--reset - just for debuging
CREATE FUNCTION reset_tsearch()
        RETURNS void
        as 'MODULE_PATHNAME'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT;

--get cover (debug for rank_cd)
CREATE FUNCTION get_covers(tsvector,tsquery)
        RETURNS text
        as 'MODULE_PATHNAME'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT;

--debug function
create type tsdebug as (
        ts_name text,
        tok_type text,
        description text,
        token   text,
        dict_name text[],
        "tsvector" tsvector
);

CREATE FUNCTION _get_parser_from_curcfg() 
RETURNS text as 
' select prs_name from pg_ts_cfg where oid = show_curcfg() '
LANGUAGE SQL RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION ts_debug(text)
RETURNS setof tsdebug as '
select 
        m.ts_name,
        t.alias as tok_type,
        t.descr as description,
        p.token,
        m.dict_name,
        strip(to_tsvector(p.token)) as tsvector
from
        parse( _get_parser_from_curcfg(), $1 ) as p,
        token_type() as t,
        pg_ts_cfgmap as m,
        pg_ts_cfg as c
where
        t.tokid=p.tokid and
        t.alias = m.tok_alias and 
        m.ts_name=c.ts_name and 
        c.oid=show_curcfg() 
' LANGUAGE SQL RETURNS NULL ON NULL INPUT;

--compare functions
CREATE FUNCTION tsvector_cmp(tsvector,tsvector)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION tsvector_lt(tsvector,tsvector)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION tsvector_le(tsvector,tsvector)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;
        
CREATE FUNCTION tsvector_eq(tsvector,tsvector)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION tsvector_ge(tsvector,tsvector)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;
        
CREATE FUNCTION tsvector_gt(tsvector,tsvector)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE FUNCTION tsvector_ne(tsvector,tsvector)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OPERATOR < (
        LEFTARG = tsvector,
        RIGHTARG = tsvector,
        PROCEDURE = tsvector_lt,
        COMMUTATOR = '>',
        NEGATOR = '>=',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR <= (
        LEFTARG = tsvector,
        RIGHTARG = tsvector,
        PROCEDURE = tsvector_le,
        COMMUTATOR = '>=',
        NEGATOR = '>',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR >= (
        LEFTARG = tsvector,
        RIGHTARG = tsvector,
        PROCEDURE = tsvector_ge,
        COMMUTATOR = '<=',
        NEGATOR = '<',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR > (
        LEFTARG = tsvector,
        RIGHTARG = tsvector,
        PROCEDURE = tsvector_gt,
        COMMUTATOR = '<',
        NEGATOR = '<=',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR = (
        LEFTARG = tsvector,
        RIGHTARG = tsvector,
        PROCEDURE = tsvector_eq,
        COMMUTATOR = '=',
        NEGATOR = '<>',
        RESTRICT = eqsel,
        JOIN = eqjoinsel,
        SORT1 = '<',
        SORT2 = '<'
);

CREATE OPERATOR <> (
        LEFTARG = tsvector,
        RIGHTARG = tsvector,
        PROCEDURE = tsvector_ne,
        COMMUTATOR = '<>',
        NEGATOR = '=',
        RESTRICT = neqsel,
        JOIN = neqjoinsel
);

CREATE OPERATOR CLASS tsvector_ops
    DEFAULT FOR TYPE tsvector USING btree AS
        OPERATOR        1       < ,
        OPERATOR        2       <= , 
        OPERATOR        3       = ,
        OPERATOR        4       >= ,
        OPERATOR        5       > ,
        FUNCTION        1       tsvector_cmp(tsvector, tsvector);

----------------Compare functions and operators for tsquery
CREATE OR REPLACE FUNCTION tsquery_cmp(tsquery,tsquery)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OR REPLACE FUNCTION tsquery_lt(tsquery,tsquery)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OR REPLACE FUNCTION tsquery_le(tsquery,tsquery)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OR REPLACE FUNCTION tsquery_eq(tsquery,tsquery)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OR REPLACE FUNCTION tsquery_ge(tsquery,tsquery)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OR REPLACE FUNCTION tsquery_gt(tsquery,tsquery)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OR REPLACE FUNCTION tsquery_ne(tsquery,tsquery)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT IMMUTABLE;


CREATE OPERATOR < (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsquery_lt,
        COMMUTATOR = '>',
        NEGATOR = '>=',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR <= (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsquery_le,
        COMMUTATOR = '>=',
        NEGATOR = '>',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR >= (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsquery_ge,
        COMMUTATOR = '<=',
        NEGATOR = '<',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR > (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsquery_gt,
        COMMUTATOR = '<',
        NEGATOR = '<=',
        RESTRICT = contsel,
        JOIN = contjoinsel
);


CREATE OPERATOR = (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsquery_eq,
        COMMUTATOR = '=',
        NEGATOR = '<>',
        RESTRICT = eqsel,
        JOIN = eqjoinsel,
        SORT1 = '<',
        SORT2 = '<'
);

CREATE OPERATOR <> (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsquery_ne,
        COMMUTATOR = '<>',
        NEGATOR = '=',
        RESTRICT = neqsel,
        JOIN = neqjoinsel
);

CREATE OPERATOR CLASS tsquery_ops
    DEFAULT FOR TYPE tsquery USING btree AS
        OPERATOR        1       < ,
        OPERATOR        2       <= ,
        OPERATOR        3       = ,
        OPERATOR        4       >= ,
        OPERATOR        5       > ,
        FUNCTION        1       tsquery_cmp(tsquery, tsquery);

CREATE OR REPLACE FUNCTION numnode(tsquery)
        RETURNS int4
        as 'MODULE_PATHNAME', 'tsquery_numnode'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OR REPLACE FUNCTION tsquery_and(tsquery,tsquery)
        RETURNS tsquery
        as 'MODULE_PATHNAME', 'tsquery_and'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OPERATOR && (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsquery_and,
        COMMUTATOR = '&&',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OR REPLACE FUNCTION tsquery_or(tsquery,tsquery)
        RETURNS tsquery
        as 'MODULE_PATHNAME', 'tsquery_or'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OPERATOR || (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsquery_or,
        COMMUTATOR = '||',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OR REPLACE FUNCTION tsquery_not(tsquery)
        RETURNS tsquery
        as 'MODULE_PATHNAME', 'tsquery_not'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OPERATOR !! (
        RIGHTARG = tsquery,
        PROCEDURE = tsquery_not
);

--------------rewrite subsystem

CREATE OR REPLACE FUNCTION rewrite(tsquery, text)
        RETURNS tsquery
        as 'MODULE_PATHNAME', 'tsquery_rewrite'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OR REPLACE FUNCTION rewrite(tsquery, tsquery, tsquery)
        RETURNS tsquery
        as 'MODULE_PATHNAME', 'tsquery_rewrite_query'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OR REPLACE FUNCTION rewrite_accum(tsquery,tsquery[])
        RETURNS tsquery
        AS 'MODULE_PATHNAME'
        LANGUAGE C;

CREATE OR REPLACE FUNCTION rewrite_finish(tsquery)
      RETURNS tsquery
      as 'MODULE_PATHNAME'
      LANGUAGE C;

CREATE AGGREGATE rewrite (
      BASETYPE=tsquery[],
      SFUNC=rewrite_accum,
      STYPE=tsquery,
      FINALFUNC = rewrite_finish
);

CREATE OR REPLACE FUNCTION tsq_mcontains(tsquery, tsquery)
        RETURNS bool
        as 'MODULE_PATHNAME'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OR REPLACE FUNCTION tsq_mcontained(tsquery, tsquery)
        RETURNS bool
        as 'MODULE_PATHNAME'
        LANGUAGE C
        RETURNS NULL ON NULL INPUT IMMUTABLE;

CREATE OPERATOR @> (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsq_mcontains,
        COMMUTATOR = '<@',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR <@ (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsq_mcontained,
        COMMUTATOR = '@>',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

-- obsolete:
CREATE OPERATOR @ (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsq_mcontains,
        COMMUTATOR = '~',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR ~ (
        LEFTARG = tsquery,
        RIGHTARG = tsquery,
        PROCEDURE = tsq_mcontained,
        COMMUTATOR = '@',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

-----------gist support of rewrite------------------

CREATE FUNCTION gtsq_in(cstring)
RETURNS gtsq
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE FUNCTION gtsq_out(gtsq)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE TYPE gtsq (
        INTERNALLENGTH = 8,
        INPUT = gtsq_in,
        OUTPUT = gtsq_out
);

CREATE FUNCTION gtsq_consistent(gtsq,internal,int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION gtsq_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION gtsq_decompress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION gtsq_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE FUNCTION gtsq_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION gtsq_union(bytea, internal)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION gtsq_same(gtsq, gtsq, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE OPERATOR CLASS gist_tp_tsquery_ops
DEFAULT FOR TYPE tsquery USING gist
AS
        OPERATOR        7       @> (tsquery, tsquery) RECHECK,
        OPERATOR        8       <@ (tsquery, tsquery) RECHECK,
        OPERATOR        13      @ (tsquery, tsquery) RECHECK,
        OPERATOR        14      ~ (tsquery, tsquery) RECHECK,
        FUNCTION        1       gtsq_consistent (gtsq, internal, int4),
        FUNCTION        2       gtsq_union (bytea, internal),
        FUNCTION        3       gtsq_compress (internal),
        FUNCTION        4       gtsq_decompress (internal),
        FUNCTION        5       gtsq_penalty (internal, internal, internal),
        FUNCTION        6       gtsq_picksplit (internal, internal),
        FUNCTION        7       gtsq_same (gtsq, gtsq, internal),
        STORAGE         gtsq;

--GIN support function
CREATE FUNCTION gin_extract_tsvector(tsvector,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE FUNCTION gin_extract_tsquery(tsquery,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE FUNCTION gin_ts_consistent(internal,internal,tsquery)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE OPERATOR @@@ (
        LEFTARG = tsvector,
        RIGHTARG = tsquery,
        PROCEDURE = exectsq,
        COMMUTATOR = '@@@',
        RESTRICT = contsel,
        JOIN = contjoinsel
);
CREATE OPERATOR @@@ (
        LEFTARG = tsquery,
        RIGHTARG = tsvector,
        PROCEDURE = rexectsq,
        COMMUTATOR = '@@@',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR CLASS gin_tsvector_ops
DEFAULT FOR TYPE tsvector USING gin
AS
        OPERATOR        1       @@ (tsvector, tsquery),
        OPERATOR        2       @@@ (tsvector, tsquery) RECHECK,
        FUNCTION        1       bttextcmp(text, text),
        FUNCTION        2       gin_extract_tsvector(tsvector,internal),
        FUNCTION        3       gin_extract_tsquery(tsquery,internal,internal),
        FUNCTION        4       gin_ts_consistent(internal,internal,tsquery),
        STORAGE         text;


--example of ISpell dictionary
--update pg_ts_dict set dict_initoption='DictFile="/usr/local/share/ispell/russian.dict" ,AffFile ="/usr/local/share/ispell/russian.aff", StopFile="/usr/local/share/ispell/russian.stop"' where dict_name='ispell_template';

--example of synonym dict
--update pg_ts_dict set dict_initoption='/usr/local/share/ispell/english.syn' where dict_name='synonym';

--example of thesaurus dict
--update pg_ts_dict set dict_initoption='DictFile="contrib/thesaurus", Dictionary="en_stem"' where dict_name='thesaurus_template';
--update pg_ts_cfgmap set dict_name = '{thesaurus_template,en_stem}' where dict_name = '{en_stem}';
END;
