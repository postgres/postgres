BEGIN;

DROP OPERATOR CLASS gist_hstore_ops USING gist CASCADE;

DROP OPERATOR ->( hstore, text );
DROP OPERATOR ||( hstore, hstore );
DROP OPERATOR @>( hstore, hstore );
DROP OPERATOR <@( hstore, hstore );
DROP OPERATOR @( hstore, hstore );
DROP OPERATOR ~( hstore, hstore );
DROP OPERATOR =>( text, text );


DROP FUNCTION fetchval(hstore,text);
DROP FUNCTION isexists(hstore,text);
DROP FUNCTION exist(hstore,text);
DROP FUNCTION isdefined(hstore,text);
DROP FUNCTION defined(hstore,text);
DROP FUNCTION delete(hstore,text);
DROP FUNCTION hs_concat(hstore,hstore);
DROP FUNCTION hs_contains(hstore,hstore);
DROP FUNCTION hs_contained(hstore,hstore);
DROP FUNCTION tconvert(text,text);
DROP FUNCTION akeys(hstore);
DROP FUNCTION avals(hstore);
DROP FUNCTION skeys(hstore);
DROP FUNCTION svals(hstore);
DROP FUNCTION each(hstore);
DROP FUNCTION ghstore_compress(internal);
DROP FUNCTION ghstore_decompress(internal);
DROP FUNCTION ghstore_penalty(internal,internal,internal);
DROP FUNCTION ghstore_picksplit(internal, internal);
DROP FUNCTION ghstore_union(internal, internal);
DROP FUNCTION ghstore_same(internal, internal, internal);
DROP FUNCTION ghstore_consistent(internal,internal,int4);

DROP TYPE hstore CASCADE;
DROP TYPE hs_each CASCADE;
DROP TYPE ghstore CASCADE;

COMMIT;
