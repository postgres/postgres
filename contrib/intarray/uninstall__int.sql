SET search_path = public;

DROP OPERATOR CLASS gin__int_ops USING gin;

DROP FUNCTION ginint4_queryextract(internal, internal, int2);

DROP FUNCTION ginint4_consistent(internal, int2, internal);

DROP OPERATOR CLASS gist__intbig_ops USING gist;

DROP FUNCTION g_intbig_same(internal, internal, internal);

DROP FUNCTION g_intbig_union(internal, internal);

DROP FUNCTION g_intbig_picksplit(internal, internal);

DROP FUNCTION g_intbig_penalty(internal,internal,internal);

DROP FUNCTION g_intbig_decompress(internal);

DROP FUNCTION g_intbig_compress(internal);

DROP FUNCTION g_intbig_consistent(internal,internal,int4);

DROP TYPE intbig_gkey CASCADE;

DROP OPERATOR CLASS gist__int_ops USING gist;

DROP FUNCTION g_int_same(_int4, _int4, internal);

DROP FUNCTION g_int_union(internal, internal);

DROP FUNCTION g_int_picksplit(internal, internal);

DROP FUNCTION g_int_penalty(internal,internal,internal);

DROP FUNCTION g_int_decompress(internal);

DROP FUNCTION g_int_compress(internal);

DROP FUNCTION g_int_consistent(internal,_int4,int4);

DROP OPERATOR & (_int4, _int4);

DROP OPERATOR - (_int4, _int4);

DROP FUNCTION intset_subtract(_int4, _int4);

DROP OPERATOR | (_int4, _int4);

DROP OPERATOR | (_int4, int4);

DROP FUNCTION intset_union_elem(_int4, int4);

DROP OPERATOR - (_int4, int4);

DROP FUNCTION intarray_del_elem(_int4, int4);

DROP OPERATOR + (_int4, _int4);

DROP FUNCTION intarray_push_array(_int4, _int4);

DROP OPERATOR + (_int4, int4);

DROP FUNCTION intarray_push_elem(_int4, int4);

DROP FUNCTION subarray(_int4, int4);

DROP FUNCTION subarray(_int4, int4, int4);

DROP OPERATOR # (_int4, int4);

DROP FUNCTION idx(_int4, int4);

DROP FUNCTION uniq(_int4);

DROP FUNCTION sort_desc(_int4);

DROP FUNCTION sort_asc(_int4);

DROP FUNCTION sort(_int4);

DROP FUNCTION sort(_int4, text);

DROP OPERATOR # (NONE, _int4);

DROP FUNCTION icount(_int4);

DROP FUNCTION intset(int4);

DROP OPERATOR <@ (_int4, _int4);

DROP OPERATOR @> (_int4, _int4);

DROP OPERATOR ~ (_int4, _int4);

DROP OPERATOR @ (_int4, _int4);

DROP OPERATOR && (_int4, _int4);

DROP FUNCTION _int_inter(_int4, _int4);

DROP FUNCTION _int_union(_int4, _int4);

DROP FUNCTION _int_different(_int4, _int4);

DROP FUNCTION _int_same(_int4, _int4);

DROP FUNCTION _int_overlap(_int4, _int4);

DROP FUNCTION _int_contained(_int4, _int4);

DROP FUNCTION _int_contains(_int4, _int4);

DROP OPERATOR ~~ (query_int, _int4);

DROP OPERATOR @@ (_int4, query_int);

DROP FUNCTION rboolop(query_int, _int4);

DROP FUNCTION boolop(_int4, query_int);

DROP FUNCTION querytree(query_int);

DROP TYPE query_int CASCADE;
