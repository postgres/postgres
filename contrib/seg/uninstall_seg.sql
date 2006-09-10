SET search_path = public;

DROP OPERATOR CLASS gist_seg_ops USING gist;

DROP OPERATOR CLASS seg_ops USING btree;

DROP FUNCTION gseg_same(seg, seg, internal);

DROP FUNCTION gseg_union(internal, internal);

DROP FUNCTION gseg_picksplit(internal, internal);

DROP FUNCTION gseg_penalty(internal,internal,internal);

DROP FUNCTION gseg_decompress(internal);

DROP FUNCTION gseg_compress(internal);

DROP FUNCTION gseg_consistent(internal,seg,int4);

DROP OPERATOR <@ (seg, seg);

DROP OPERATOR @> (seg, seg);

DROP OPERATOR ~ (seg, seg);

DROP OPERATOR @ (seg, seg);

DROP OPERATOR <> (seg, seg);

DROP OPERATOR = (seg, seg);

DROP OPERATOR >> (seg, seg);

DROP OPERATOR &> (seg, seg);

DROP OPERATOR && (seg, seg);

DROP OPERATOR &< (seg, seg);

DROP OPERATOR << (seg, seg);

DROP OPERATOR >= (seg, seg);

DROP OPERATOR > (seg, seg);

DROP OPERATOR <= (seg, seg);

DROP OPERATOR < (seg, seg);

DROP FUNCTION seg_lower(seg);

DROP FUNCTION seg_upper(seg);

DROP FUNCTION seg_size(seg);

DROP FUNCTION seg_inter(seg, seg);

DROP FUNCTION seg_union(seg, seg);

DROP FUNCTION seg_cmp(seg, seg);

DROP FUNCTION seg_different(seg, seg);

DROP FUNCTION seg_same(seg, seg);

DROP FUNCTION seg_overlap(seg, seg);

DROP FUNCTION seg_contained(seg, seg);

DROP FUNCTION seg_contains(seg, seg);

DROP FUNCTION seg_ge(seg, seg);

DROP FUNCTION seg_gt(seg, seg);

DROP FUNCTION seg_le(seg, seg);

DROP FUNCTION seg_lt(seg, seg);

DROP FUNCTION seg_right(seg, seg);

DROP FUNCTION seg_left(seg, seg);

DROP FUNCTION seg_over_right(seg, seg);

DROP FUNCTION seg_over_left(seg, seg);

DROP TYPE seg CASCADE;
