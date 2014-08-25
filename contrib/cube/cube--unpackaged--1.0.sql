/* contrib/cube/cube--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION cube FROM unpackaged" to load this file. \quit

ALTER EXTENSION cube ADD type cube;
ALTER EXTENSION cube ADD function cube_in(cstring);
ALTER EXTENSION cube ADD function cube(double precision[],double precision[]);
ALTER EXTENSION cube ADD function cube(double precision[]);
ALTER EXTENSION cube ADD function cube_out(cube);
ALTER EXTENSION cube ADD function cube_eq(cube,cube);
ALTER EXTENSION cube ADD function cube_ne(cube,cube);
ALTER EXTENSION cube ADD function cube_lt(cube,cube);
ALTER EXTENSION cube ADD function cube_gt(cube,cube);
ALTER EXTENSION cube ADD function cube_le(cube,cube);
ALTER EXTENSION cube ADD function cube_ge(cube,cube);
ALTER EXTENSION cube ADD function cube_cmp(cube,cube);
ALTER EXTENSION cube ADD function cube_contains(cube,cube);
ALTER EXTENSION cube ADD function cube_contained(cube,cube);
ALTER EXTENSION cube ADD function cube_overlap(cube,cube);
ALTER EXTENSION cube ADD function cube_union(cube,cube);
ALTER EXTENSION cube ADD function cube_inter(cube,cube);
ALTER EXTENSION cube ADD function cube_size(cube);
ALTER EXTENSION cube ADD function cube_subset(cube,integer[]);
ALTER EXTENSION cube ADD function cube_distance(cube,cube);
ALTER EXTENSION cube ADD function cube_dim(cube);
ALTER EXTENSION cube ADD function cube_ll_coord(cube,integer);
ALTER EXTENSION cube ADD function cube_ur_coord(cube,integer);
ALTER EXTENSION cube ADD function cube(double precision);
ALTER EXTENSION cube ADD function cube(double precision,double precision);
ALTER EXTENSION cube ADD function cube(cube,double precision);
ALTER EXTENSION cube ADD function cube(cube,double precision,double precision);
ALTER EXTENSION cube ADD function cube_is_point(cube);
ALTER EXTENSION cube ADD function cube_enlarge(cube,double precision,integer);
ALTER EXTENSION cube ADD operator >(cube,cube);
ALTER EXTENSION cube ADD operator >=(cube,cube);
ALTER EXTENSION cube ADD operator <(cube,cube);
ALTER EXTENSION cube ADD operator <=(cube,cube);
ALTER EXTENSION cube ADD operator &&(cube,cube);
ALTER EXTENSION cube ADD operator <>(cube,cube);
ALTER EXTENSION cube ADD operator =(cube,cube);
ALTER EXTENSION cube ADD operator <@(cube,cube);
ALTER EXTENSION cube ADD operator @>(cube,cube);
ALTER EXTENSION cube ADD operator ~(cube,cube);
ALTER EXTENSION cube ADD operator @(cube,cube);
ALTER EXTENSION cube ADD function g_cube_consistent(internal,cube,integer,oid,internal);
ALTER EXTENSION cube ADD function g_cube_compress(internal);
ALTER EXTENSION cube ADD function g_cube_decompress(internal);
ALTER EXTENSION cube ADD function g_cube_penalty(internal,internal,internal);
ALTER EXTENSION cube ADD function g_cube_picksplit(internal,internal);
ALTER EXTENSION cube ADD function g_cube_union(internal,internal);
ALTER EXTENSION cube ADD function g_cube_same(cube,cube,internal);
ALTER EXTENSION cube ADD operator family cube_ops using btree;
ALTER EXTENSION cube ADD operator class cube_ops using btree;
ALTER EXTENSION cube ADD operator family gist_cube_ops using gist;
ALTER EXTENSION cube ADD operator class gist_cube_ops using gist;
