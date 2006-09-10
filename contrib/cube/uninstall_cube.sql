SET search_path = public;

DROP OPERATOR CLASS gist_cube_ops USING gist;

DROP OPERATOR CLASS cube_ops USING btree;

DROP FUNCTION g_cube_same(cube, cube, internal);

DROP FUNCTION g_cube_union(internal, internal);

DROP FUNCTION g_cube_picksplit(internal, internal);

DROP FUNCTION g_cube_penalty(internal,internal,internal);

DROP FUNCTION g_cube_decompress(internal);

DROP FUNCTION g_cube_compress(internal);

DROP FUNCTION g_cube_consistent(internal,cube,int4);

DROP OPERATOR ~ (cube, cube);

DROP OPERATOR @ (cube, cube);

DROP OPERATOR <@ (cube, cube);

DROP OPERATOR @> (cube, cube);

DROP OPERATOR <> (cube, cube);

DROP OPERATOR = (cube, cube);

DROP OPERATOR && (cube, cube);

DROP OPERATOR >= (cube, cube);

DROP OPERATOR <= (cube, cube);

DROP OPERATOR > (cube, cube);

DROP OPERATOR < (cube, cube);

DROP FUNCTION cube_enlarge(cube, float8, int4);

DROP FUNCTION cube_is_point(cube);

DROP FUNCTION cube(cube, float8, float8);

DROP FUNCTION cube(cube, float8);

DROP FUNCTION cube(float8, float8);

DROP FUNCTION cube(float8[], float8[]);

DROP FUNCTION cube(float8[]);

DROP FUNCTION cube_subset(cube, int4[]);

DROP FUNCTION cube(float8);

DROP FUNCTION cube_ur_coord(cube, int4);

DROP FUNCTION cube_ll_coord(cube, int4);

DROP FUNCTION cube_dim(cube);

DROP FUNCTION cube_distance(cube, cube);

DROP FUNCTION cube_size(cube);

DROP FUNCTION cube_inter(cube, cube);

DROP FUNCTION cube_union(cube, cube);

DROP FUNCTION cube_overlap(cube, cube);

DROP FUNCTION cube_contained(cube, cube);

DROP FUNCTION cube_contains(cube, cube);

DROP FUNCTION cube_cmp(cube, cube);

DROP FUNCTION cube_ge(cube, cube);

DROP FUNCTION cube_le(cube, cube);

DROP FUNCTION cube_gt(cube, cube);

DROP FUNCTION cube_lt(cube, cube);

DROP FUNCTION cube_ne(cube, cube);

DROP FUNCTION cube_eq(cube, cube);

DROP CAST (text AS cube);

DROP FUNCTION cube(text);

DROP TYPE cube CASCADE;
