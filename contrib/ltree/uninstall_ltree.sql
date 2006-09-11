SET search_path = public;

DROP OPERATOR CLASS gist__ltree_ops USING gist;

DROP FUNCTION _ltree_same(internal, internal, internal);

DROP FUNCTION _ltree_union(internal, internal);

DROP FUNCTION _ltree_picksplit(internal, internal);

DROP FUNCTION _ltree_penalty(internal,internal,internal);

DROP FUNCTION _ltree_compress(internal);

DROP FUNCTION _ltree_consistent(internal,internal,int2);

DROP OPERATOR ?@ (_ltree, ltxtquery);

DROP FUNCTION _ltxtq_extract_exec(_ltree,ltxtquery);

DROP OPERATOR ?~ (_ltree, lquery);

DROP FUNCTION _ltq_extract_regex(_ltree,lquery);

DROP OPERATOR ?<@ (_ltree, ltree);

DROP FUNCTION _ltree_extract_risparent(_ltree,ltree);

DROP OPERATOR ?@> (_ltree, ltree);

DROP FUNCTION _ltree_extract_isparent(_ltree,ltree);

DROP OPERATOR ^@ (ltxtquery, _ltree);

DROP OPERATOR ^@ (_ltree, ltxtquery);

DROP OPERATOR ^? (_lquery, _ltree);

DROP OPERATOR ^? (_ltree, _lquery);

DROP OPERATOR ^~ (lquery, _ltree);

DROP OPERATOR ^~ (_ltree, lquery);

DROP OPERATOR ^@> (ltree, _ltree);

DROP OPERATOR ^<@ (_ltree, ltree);

DROP OPERATOR ^<@ (ltree, _ltree);

DROP OPERATOR ^@> (_ltree, ltree);

DROP OPERATOR @ (ltxtquery, _ltree);

DROP OPERATOR @ (_ltree, ltxtquery);

DROP OPERATOR ? (_lquery, _ltree);

DROP OPERATOR ? (_ltree, _lquery);

DROP OPERATOR ~ (lquery, _ltree);

DROP OPERATOR ~ (_ltree, lquery);

DROP OPERATOR @> (ltree, _ltree);

DROP OPERATOR <@ (_ltree, ltree);

DROP OPERATOR <@ (ltree, _ltree);

DROP OPERATOR @> (_ltree, ltree);

DROP FUNCTION _ltxtq_rexec(ltxtquery, _ltree);

DROP FUNCTION _ltxtq_exec(_ltree, ltxtquery);

DROP FUNCTION _lt_q_rregex(_lquery,_ltree);

DROP FUNCTION _lt_q_regex(_ltree,_lquery);

DROP FUNCTION _ltq_rregex(lquery,_ltree);

DROP FUNCTION _ltq_regex(_ltree,lquery);

DROP FUNCTION _ltree_r_risparent(ltree,_ltree);

DROP FUNCTION _ltree_risparent(_ltree,ltree);

DROP FUNCTION _ltree_r_isparent(ltree,_ltree);

DROP FUNCTION _ltree_isparent(_ltree,ltree);

DROP OPERATOR CLASS gist_ltree_ops USING gist;

DROP FUNCTION ltree_same(internal, internal, internal);

DROP FUNCTION ltree_union(internal, internal);

DROP FUNCTION ltree_picksplit(internal, internal);

DROP FUNCTION ltree_penalty(internal,internal,internal);

DROP FUNCTION ltree_decompress(internal);

DROP FUNCTION ltree_compress(internal);

DROP FUNCTION ltree_consistent(internal,internal,int2);

DROP TYPE ltree_gist CASCADE;
  
DROP OPERATOR ^@ (ltxtquery, ltree);

DROP OPERATOR ^@ (ltree, ltxtquery);

DROP OPERATOR @ (ltxtquery, ltree);

DROP OPERATOR @ (ltree, ltxtquery);

DROP FUNCTION ltxtq_rexec(ltxtquery, ltree);

DROP FUNCTION ltxtq_exec(ltree, ltxtquery);

DROP TYPE ltxtquery CASCADE;

DROP OPERATOR ^? (_lquery, ltree);

DROP OPERATOR ^? (ltree, _lquery);

DROP OPERATOR ? (_lquery, ltree);

DROP OPERATOR ? (ltree, _lquery);

DROP FUNCTION lt_q_rregex(_lquery,ltree);

DROP FUNCTION lt_q_regex(ltree,_lquery);

DROP OPERATOR ^~ (lquery, ltree);

DROP OPERATOR ^~ (ltree, lquery);

DROP OPERATOR ~ (lquery, ltree);

DROP OPERATOR ~ (ltree, lquery);

DROP FUNCTION ltq_rregex(lquery,ltree);

DROP FUNCTION ltq_regex(ltree,lquery);

DROP TYPE lquery CASCADE;

DROP OPERATOR CLASS ltree_ops USING btree;

DROP OPERATOR || (text, ltree);

DROP OPERATOR || (ltree, text);

DROP OPERATOR || (ltree, ltree);

DROP OPERATOR ^<@ (ltree, ltree);

DROP OPERATOR <@ (ltree, ltree);

DROP OPERATOR ^@> (ltree, ltree);

DROP OPERATOR @> (ltree, ltree);

DROP FUNCTION ltreeparentsel(internal, oid, internal, integer);

DROP FUNCTION ltree_textadd(text,ltree);

DROP FUNCTION ltree_addtext(ltree,text);

DROP FUNCTION ltree_addltree(ltree,ltree);

DROP FUNCTION ltree_risparent(ltree,ltree);

DROP FUNCTION ltree_isparent(ltree,ltree);

DROP FUNCTION lca(ltree,ltree,ltree,ltree,ltree,ltree,ltree,ltree);

DROP FUNCTION lca(ltree,ltree,ltree,ltree,ltree,ltree,ltree);

DROP FUNCTION lca(ltree,ltree,ltree,ltree,ltree,ltree);

DROP FUNCTION lca(ltree,ltree,ltree,ltree,ltree);

DROP FUNCTION lca(ltree,ltree,ltree,ltree);

DROP FUNCTION lca(ltree,ltree,ltree);

DROP FUNCTION lca(ltree,ltree);

DROP FUNCTION lca(_ltree);

DROP FUNCTION text2ltree(text);

DROP FUNCTION ltree2text(ltree);

DROP FUNCTION nlevel(ltree);

DROP FUNCTION index(ltree,ltree,int4);

DROP FUNCTION index(ltree,ltree);

DROP FUNCTION subpath(ltree,int4);

DROP FUNCTION subpath(ltree,int4,int4);

DROP FUNCTION subltree(ltree,int4,int4);

DROP OPERATOR <> (ltree, ltree);

DROP OPERATOR = (ltree, ltree);

DROP OPERATOR > (ltree, ltree);

DROP OPERATOR >= (ltree, ltree);

DROP OPERATOR <= (ltree, ltree);

DROP OPERATOR < (ltree, ltree);

DROP FUNCTION ltree_ne(ltree,ltree);

DROP FUNCTION ltree_gt(ltree,ltree);

DROP FUNCTION ltree_ge(ltree,ltree);

DROP FUNCTION ltree_eq(ltree,ltree);

DROP FUNCTION ltree_le(ltree,ltree);

DROP FUNCTION ltree_lt(ltree,ltree);

DROP FUNCTION ltree_cmp(ltree,ltree);

DROP TYPE ltree CASCADE;
