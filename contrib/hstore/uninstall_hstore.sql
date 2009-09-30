/* $PostgreSQL: pgsql/contrib/hstore/uninstall_hstore.sql,v 1.9 2009/09/30 19:50:22 tgl Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP OPERATOR CLASS gist_hstore_ops USING gist CASCADE;
DROP OPERATOR CLASS gin_hstore_ops USING gin CASCADE;
DROP OPERATOR CLASS hash_hstore_ops USING hash CASCADE;
DROP OPERATOR CLASS btree_hstore_ops USING btree CASCADE;

DROP OPERATOR -  ( hstore, text );
DROP OPERATOR -  ( hstore, text[] );
DROP OPERATOR -  ( hstore, hstore );
DROP OPERATOR ?  ( hstore, text );
DROP OPERATOR ?& ( hstore, text[] );
DROP OPERATOR ?| ( hstore, text[] );
DROP OPERATOR -> ( hstore, text );
DROP OPERATOR -> ( hstore, text[] );
DROP OPERATOR || ( hstore, hstore );
DROP OPERATOR @> ( hstore, hstore );
DROP OPERATOR <@ ( hstore, hstore );
DROP OPERATOR @  ( hstore, hstore );
DROP OPERATOR ~  ( hstore, hstore );
DROP OPERATOR => ( text, text );
DROP OPERATOR => ( text[], text[] );
DROP OPERATOR => ( hstore, text[] );
DROP OPERATOR #= ( anyelement, hstore );
DROP OPERATOR %% ( NONE, hstore );
DROP OPERATOR %# ( NONE, hstore );
DROP OPERATOR =  ( hstore, hstore );
DROP OPERATOR <> ( hstore, hstore );
DROP OPERATOR #<#  ( hstore, hstore );
DROP OPERATOR #<=# ( hstore, hstore );
DROP OPERATOR #>#  ( hstore, hstore );
DROP OPERATOR #>=# ( hstore, hstore );

DROP CAST (text[] AS hstore);

DROP FUNCTION hstore_eq(hstore,hstore);
DROP FUNCTION hstore_ne(hstore,hstore);
DROP FUNCTION hstore_gt(hstore,hstore);
DROP FUNCTION hstore_ge(hstore,hstore);
DROP FUNCTION hstore_lt(hstore,hstore);
DROP FUNCTION hstore_le(hstore,hstore);
DROP FUNCTION hstore_cmp(hstore,hstore);
DROP FUNCTION hstore_hash(hstore);
DROP FUNCTION slice_array(hstore,text[]);
DROP FUNCTION slice_hstore(hstore,text[]);
DROP FUNCTION fetchval(hstore,text);
DROP FUNCTION isexists(hstore,text);
DROP FUNCTION exist(hstore,text);
DROP FUNCTION exists_any(hstore,text[]);
DROP FUNCTION exists_all(hstore,text[]);
DROP FUNCTION isdefined(hstore,text);
DROP FUNCTION defined(hstore,text);
DROP FUNCTION delete(hstore,text);
DROP FUNCTION delete(hstore,text[]);
DROP FUNCTION delete(hstore,hstore);
DROP FUNCTION hs_concat(hstore,hstore);
DROP FUNCTION hs_contains(hstore,hstore);
DROP FUNCTION hs_contained(hstore,hstore);
DROP FUNCTION tconvert(text,text);
DROP FUNCTION hstore(text,text);
DROP FUNCTION hstore(text[],text[]);
DROP FUNCTION hstore_to_array(hstore);
DROP FUNCTION hstore_to_matrix(hstore);
DROP FUNCTION hstore(record);
DROP FUNCTION hstore(text[]);
DROP FUNCTION akeys(hstore);
DROP FUNCTION avals(hstore);
DROP FUNCTION skeys(hstore);
DROP FUNCTION svals(hstore);
DROP FUNCTION each(hstore);
DROP FUNCTION populate_record(anyelement,hstore);
DROP FUNCTION ghstore_compress(internal);
DROP FUNCTION ghstore_decompress(internal);
DROP FUNCTION ghstore_penalty(internal,internal,internal);
DROP FUNCTION ghstore_picksplit(internal, internal);
DROP FUNCTION ghstore_union(internal, internal);
DROP FUNCTION ghstore_same(internal, internal, internal);
DROP FUNCTION ghstore_consistent(internal,internal,int,oid,internal);
DROP FUNCTION gin_consistent_hstore(internal, int2, internal, int4, internal, internal);
DROP FUNCTION gin_extract_hstore(internal, internal);
DROP FUNCTION gin_extract_hstore_query(internal, internal, smallint, internal, internal);
DROP FUNCTION hstore_version_diag(hstore);

DROP TYPE hstore CASCADE;
DROP TYPE ghstore CASCADE;
