/* contrib/pg_trgm/pg_trgm--unpackaged--1.0.sql */

ALTER EXTENSION pg_trgm ADD function set_limit(real);
ALTER EXTENSION pg_trgm ADD function show_limit();
ALTER EXTENSION pg_trgm ADD function show_trgm(text);
ALTER EXTENSION pg_trgm ADD function similarity(text,text);
ALTER EXTENSION pg_trgm ADD function similarity_op(text,text);
ALTER EXTENSION pg_trgm ADD operator %(text,text);
ALTER EXTENSION pg_trgm ADD function similarity_dist(text,text);
ALTER EXTENSION pg_trgm ADD operator <->(text,text);
ALTER EXTENSION pg_trgm ADD type gtrgm;
ALTER EXTENSION pg_trgm ADD function gtrgm_in(cstring);
ALTER EXTENSION pg_trgm ADD function gtrgm_out(gtrgm);
ALTER EXTENSION pg_trgm ADD function gtrgm_consistent(internal,text,integer,oid,internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_distance(internal,text,integer,oid);
ALTER EXTENSION pg_trgm ADD function gtrgm_compress(internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_decompress(internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_penalty(internal,internal,internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_picksplit(internal,internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_union(bytea,internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_same(gtrgm,gtrgm,internal);
ALTER EXTENSION pg_trgm ADD operator family gist_trgm_ops using gist;
ALTER EXTENSION pg_trgm ADD operator class gist_trgm_ops using gist;
ALTER EXTENSION pg_trgm ADD function gin_extract_value_trgm(text,internal);
ALTER EXTENSION pg_trgm ADD function gin_extract_query_trgm(text,internal,smallint,internal,internal,internal,internal);
ALTER EXTENSION pg_trgm ADD function gin_trgm_consistent(internal,smallint,text,integer,internal,internal,internal,internal);
ALTER EXTENSION pg_trgm ADD operator family gin_trgm_ops using gin;
ALTER EXTENSION pg_trgm ADD operator class gin_trgm_ops using gin;
