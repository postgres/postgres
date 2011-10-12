/* contrib/seg/seg--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION seg" to load this file. \quit

ALTER EXTENSION seg ADD type seg;
ALTER EXTENSION seg ADD function seg_in(cstring);
ALTER EXTENSION seg ADD function seg_out(seg);
ALTER EXTENSION seg ADD function seg_over_left(seg,seg);
ALTER EXTENSION seg ADD function seg_over_right(seg,seg);
ALTER EXTENSION seg ADD function seg_left(seg,seg);
ALTER EXTENSION seg ADD function seg_right(seg,seg);
ALTER EXTENSION seg ADD function seg_lt(seg,seg);
ALTER EXTENSION seg ADD function seg_le(seg,seg);
ALTER EXTENSION seg ADD function seg_gt(seg,seg);
ALTER EXTENSION seg ADD function seg_ge(seg,seg);
ALTER EXTENSION seg ADD function seg_contains(seg,seg);
ALTER EXTENSION seg ADD function seg_contained(seg,seg);
ALTER EXTENSION seg ADD function seg_overlap(seg,seg);
ALTER EXTENSION seg ADD function seg_same(seg,seg);
ALTER EXTENSION seg ADD function seg_different(seg,seg);
ALTER EXTENSION seg ADD function seg_cmp(seg,seg);
ALTER EXTENSION seg ADD function seg_union(seg,seg);
ALTER EXTENSION seg ADD function seg_inter(seg,seg);
ALTER EXTENSION seg ADD function seg_size(seg);
ALTER EXTENSION seg ADD function seg_center(seg);
ALTER EXTENSION seg ADD function seg_upper(seg);
ALTER EXTENSION seg ADD function seg_lower(seg);
ALTER EXTENSION seg ADD operator >(seg,seg);
ALTER EXTENSION seg ADD operator >=(seg,seg);
ALTER EXTENSION seg ADD operator <(seg,seg);
ALTER EXTENSION seg ADD operator <=(seg,seg);
ALTER EXTENSION seg ADD operator >>(seg,seg);
ALTER EXTENSION seg ADD operator <<(seg,seg);
ALTER EXTENSION seg ADD operator &<(seg,seg);
ALTER EXTENSION seg ADD operator &&(seg,seg);
ALTER EXTENSION seg ADD operator &>(seg,seg);
ALTER EXTENSION seg ADD operator <>(seg,seg);
ALTER EXTENSION seg ADD operator =(seg,seg);
ALTER EXTENSION seg ADD operator <@(seg,seg);
ALTER EXTENSION seg ADD operator @>(seg,seg);
ALTER EXTENSION seg ADD operator ~(seg,seg);
ALTER EXTENSION seg ADD operator @(seg,seg);
ALTER EXTENSION seg ADD function gseg_consistent(internal,seg,integer,oid,internal);
ALTER EXTENSION seg ADD function gseg_compress(internal);
ALTER EXTENSION seg ADD function gseg_decompress(internal);
ALTER EXTENSION seg ADD function gseg_penalty(internal,internal,internal);
ALTER EXTENSION seg ADD function gseg_picksplit(internal,internal);
ALTER EXTENSION seg ADD function gseg_union(internal,internal);
ALTER EXTENSION seg ADD function gseg_same(seg,seg,internal);
ALTER EXTENSION seg ADD operator family seg_ops using btree;
ALTER EXTENSION seg ADD operator class seg_ops using btree;
ALTER EXTENSION seg ADD operator family gist_seg_ops using gist;
ALTER EXTENSION seg ADD operator class gist_seg_ops using gist;
