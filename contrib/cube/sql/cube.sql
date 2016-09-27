--
--  Test cube datatype
--

CREATE EXTENSION cube;

--
-- testing the input and output functions
--

-- Any number (a one-dimensional point)
SELECT '1'::cube AS cube;
SELECT '-1'::cube AS cube;
SELECT '1.'::cube AS cube;
SELECT '-1.'::cube AS cube;
SELECT '.1'::cube AS cube;
SELECT '-.1'::cube AS cube;
SELECT '1.0'::cube AS cube;
SELECT '-1.0'::cube AS cube;
SELECT '1e27'::cube AS cube;
SELECT '-1e27'::cube AS cube;
SELECT '1.0e27'::cube AS cube;
SELECT '-1.0e27'::cube AS cube;
SELECT '1e+27'::cube AS cube;
SELECT '-1e+27'::cube AS cube;
SELECT '1.0e+27'::cube AS cube;
SELECT '-1.0e+27'::cube AS cube;
SELECT '1e-7'::cube AS cube;
SELECT '-1e-7'::cube AS cube;
SELECT '1.0e-7'::cube AS cube;
SELECT '-1.0e-7'::cube AS cube;
SELECT '1e-300'::cube AS cube;
SELECT '-1e-300'::cube AS cube;
SELECT 'infinity'::cube AS cube;
SELECT '-infinity'::cube AS cube;
SELECT 'NaN'::cube AS cube;
SELECT '1234567890123456'::cube AS cube;
SELECT '+1234567890123456'::cube AS cube;
SELECT '-1234567890123456'::cube AS cube;
SELECT '.1234567890123456'::cube AS cube;
SELECT '+.1234567890123456'::cube AS cube;
SELECT '-.1234567890123456'::cube AS cube;

-- simple lists (points)
SELECT '()'::cube AS cube;
SELECT '1,2'::cube AS cube;
SELECT '(1,2)'::cube AS cube;
SELECT '1,2,3,4,5'::cube AS cube;
SELECT '(1,2,3,4,5)'::cube AS cube;

-- double lists (cubes)
SELECT '(),()'::cube AS cube;
SELECT '(0),(0)'::cube AS cube;
SELECT '(0),(1)'::cube AS cube;
SELECT '[(0),(0)]'::cube AS cube;
SELECT '[(0),(1)]'::cube AS cube;
SELECT '(0,0,0,0),(0,0,0,0)'::cube AS cube;
SELECT '(0,0,0,0),(1,0,0,0)'::cube AS cube;
SELECT '[(0,0,0,0),(0,0,0,0)]'::cube AS cube;
SELECT '[(0,0,0,0),(1,0,0,0)]'::cube AS cube;

-- invalid input: parse errors
SELECT ''::cube AS cube;
SELECT 'ABC'::cube AS cube;
SELECT '[]'::cube AS cube;
SELECT '[()]'::cube AS cube;
SELECT '[(1)]'::cube AS cube;
SELECT '[(1),]'::cube AS cube;
SELECT '[(1),2]'::cube AS cube;
SELECT '[(1),(2),(3)]'::cube AS cube;
SELECT '1,'::cube AS cube;
SELECT '1,2,'::cube AS cube;
SELECT '1,,2'::cube AS cube;
SELECT '(1,)'::cube AS cube;
SELECT '(1,2,)'::cube AS cube;
SELECT '(1,,2)'::cube AS cube;

-- invalid input: semantic errors and trailing garbage
SELECT '[(1),(2)],'::cube AS cube; -- 0
SELECT '[(1,2,3),(2,3)]'::cube AS cube; -- 1
SELECT '[(1,2),(1,2,3)]'::cube AS cube; -- 1
SELECT '(1),(2),'::cube AS cube; -- 2
SELECT '(1,2,3),(2,3)'::cube AS cube; -- 3
SELECT '(1,2),(1,2,3)'::cube AS cube; -- 3
SELECT '(1,2,3)ab'::cube AS cube; -- 4
SELECT '(1,2,3)a'::cube AS cube; -- 5
SELECT '(1,2)('::cube AS cube; -- 5
SELECT '1,2ab'::cube AS cube; -- 6
SELECT '1 e7'::cube AS cube; -- 6
SELECT '1,2a'::cube AS cube; -- 7
SELECT '1..2'::cube AS cube; -- 7
SELECT '-1e-700'::cube AS cube; -- out of range

--
-- Testing building cubes from float8 values
--

SELECT cube(0::float8);
SELECT cube(1::float8);
SELECT cube(1,2);
SELECT cube(cube(1,2),3);
SELECT cube(cube(1,2),3,4);
SELECT cube(cube(cube(1,2),3,4),5);
SELECT cube(cube(cube(1,2),3,4),5,6);

--
-- Test that the text -> cube cast was installed.
--

SELECT '(0)'::text::cube;

--
-- Test the float[] -> cube cast
--
SELECT cube('{0,1,2}'::float[], '{3,4,5}'::float[]);
SELECT cube('{0,1,2}'::float[], '{3}'::float[]);
SELECT cube(NULL::float[], '{3}'::float[]);
SELECT cube('{0,1,2}'::float[]);
SELECT cube_subset(cube('(1,3,5),(6,7,8)'), ARRAY[3,2,1,1]);
SELECT cube_subset(cube('(1,3,5),(1,3,5)'), ARRAY[3,2,1,1]);
SELECT cube_subset(cube('(1,3,5),(6,7,8)'), ARRAY[4,0]);
SELECT cube_subset(cube('(6,7,8),(6,7,8)'), ARRAY[4,0]);

--
-- Test point processing
--
SELECT cube('(1,2),(1,2)'); -- cube_in
SELECT cube('{0,1,2}'::float[], '{0,1,2}'::float[]); -- cube_a_f8_f8
SELECT cube('{5,6,7,8}'::float[]); -- cube_a_f8
SELECT cube(1.37); -- cube_f8
SELECT cube(1.37, 1.37); -- cube_f8_f8
SELECT cube(cube(1,1), 42); -- cube_c_f8
SELECT cube(cube(1,2), 42); -- cube_c_f8
SELECT cube(cube(1,1), 42, 42); -- cube_c_f8_f8
SELECT cube(cube(1,1), 42, 24); -- cube_c_f8_f8
SELECT cube(cube(1,2), 42, 42); -- cube_c_f8_f8
SELECT cube(cube(1,2), 42, 24); -- cube_c_f8_f8

--
-- Testing limit of CUBE_MAX_DIM dimensions check in cube_in.
--

select '(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)'::cube;
select '(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0),(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)'::cube;

--
-- testing the  operators
--

-- equality/inequality:
--
SELECT '24, 33.20'::cube    =  '24, 33.20'::cube AS bool;
SELECT '24, 33.20'::cube    != '24, 33.20'::cube AS bool;
SELECT '24, 33.20'::cube    =  '24, 33.21'::cube AS bool;
SELECT '24, 33.20'::cube    != '24, 33.21'::cube AS bool;
SELECT '(2,0),(3,1)'::cube  =  '(2,0,0,0,0),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube  =  '(2,0,0,0,0),(3,1,0,0,1)'::cube AS bool;

-- "lower than" / "greater than"
-- (these operators are not useful for anything but ordering)
--
SELECT '1'::cube   > '2'::cube AS bool;
SELECT '1'::cube   < '2'::cube AS bool;
SELECT '1,1'::cube > '1,2'::cube AS bool;
SELECT '1,1'::cube < '1,2'::cube AS bool;

SELECT '(2,0),(3,1)'::cube             > '(2,0,0,0,0),(3,1,0,0,1)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             < '(2,0,0,0,0),(3,1,0,0,1)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             > '(2,0,0,0,1),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             < '(2,0,0,0,1),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             > '(2,0,0,0,0),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             < '(2,0,0,0,0),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,1)'::cube > '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,1)'::cube < '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,1),(3,1,0,0,0)'::cube > '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,1),(3,1,0,0,0)'::cube < '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,0)'::cube > '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,0)'::cube < '(2,0),(3,1)'::cube AS bool;


-- "overlap"
--
SELECT '1'::cube && '1'::cube AS bool;
SELECT '1'::cube && '2'::cube AS bool;

SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '0'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '1'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '1,1,1'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '[(1,1,1),(2,2,2)]'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '[(1,1),(2,2)]'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '[(2,1,1),(2,2,2)]'::cube AS bool;


-- "contained in" (the left operand is the cube entirely enclosed by
-- the right operand):
--
SELECT '0'::cube                 <@ '0'::cube                        AS bool;
SELECT '0,0,0'::cube             <@ '0,0,0'::cube                    AS bool;
SELECT '0,0'::cube               <@ '0,0,1'::cube                    AS bool;
SELECT '0,0,0'::cube             <@ '0,0,1'::cube                    AS bool;
SELECT '1,0,0'::cube             <@ '0,0,1'::cube                    AS bool;
SELECT '(1,0,0),(0,0,1)'::cube   <@ '(1,0,0),(0,0,1)'::cube          AS bool;
SELECT '(1,0,0),(0,0,1)'::cube   <@ '(-1,-1,-1),(1,1,1)'::cube       AS bool;
SELECT '(1,0,0),(0,0,1)'::cube   <@ '(-1,-1,-1,-1),(1,1,1,1)'::cube  AS bool;
SELECT '0'::cube                 <@ '(-1),(1)'::cube                 AS bool;
SELECT '1'::cube                 <@ '(-1),(1)'::cube                 AS bool;
SELECT '-1'::cube                <@ '(-1),(1)'::cube                 AS bool;
SELECT '(-1),(1)'::cube          <@ '(-1),(1)'::cube                 AS bool;
SELECT '(-1),(1)'::cube          <@ '(-1,-1),(1,1)'::cube            AS bool;
SELECT '(-2),(1)'::cube          <@ '(-1),(1)'::cube                 AS bool;
SELECT '(-2),(1)'::cube          <@ '(-1,-1),(1,1)'::cube            AS bool;


-- "contains" (the left operand is the cube that entirely encloses the
-- right operand)
--
SELECT '0'::cube                        @> '0'::cube                 AS bool;
SELECT '0,0,0'::cube                    @> '0,0,0'::cube             AS bool;
SELECT '0,0,1'::cube                    @> '0,0'::cube               AS bool;
SELECT '0,0,1'::cube                    @> '0,0,0'::cube             AS bool;
SELECT '0,0,1'::cube                    @> '1,0,0'::cube             AS bool;
SELECT '(1,0,0),(0,0,1)'::cube          @> '(1,0,0),(0,0,1)'::cube   AS bool;
SELECT '(-1,-1,-1),(1,1,1)'::cube       @> '(1,0,0),(0,0,1)'::cube   AS bool;
SELECT '(-1,-1,-1,-1),(1,1,1,1)'::cube  @> '(1,0,0),(0,0,1)'::cube   AS bool;
SELECT '(-1),(1)'::cube                 @> '0'::cube                 AS bool;
SELECT '(-1),(1)'::cube                 @> '1'::cube                 AS bool;
SELECT '(-1),(1)'::cube                 @> '-1'::cube                AS bool;
SELECT '(-1),(1)'::cube                 @> '(-1),(1)'::cube          AS bool;
SELECT '(-1,-1),(1,1)'::cube            @> '(-1),(1)'::cube          AS bool;
SELECT '(-1),(1)'::cube                 @> '(-2),(1)'::cube          AS bool;
SELECT '(-1,-1),(1,1)'::cube            @> '(-2),(1)'::cube          AS bool;

-- Test of distance function
--
SELECT cube_distance('(0)'::cube,'(2,2,2,2)'::cube);
SELECT cube_distance('(0)'::cube,'(.3,.4)'::cube);
SELECT cube_distance('(2,3,4)'::cube,'(2,3,4)'::cube);
SELECT cube_distance('(42,42,42,42)'::cube,'(137,137,137,137)'::cube);
SELECT cube_distance('(42,42,42)'::cube,'(137,137)'::cube);

-- Test of cube function (text to cube)
--
SELECT cube('(1,1.2)'::text);
SELECT cube(NULL);

-- Test of cube_dim function (dimensions stored in cube)
--
SELECT cube_dim('(0)'::cube);
SELECT cube_dim('(0,0)'::cube);
SELECT cube_dim('(0,0,0)'::cube);
SELECT cube_dim('(42,42,42),(42,42,42)'::cube);
SELECT cube_dim('(4,8,15,16,23),(4,8,15,16,23)'::cube);

-- Test of cube_ll_coord function (retrieves LL coodinate values)
--
SELECT cube_ll_coord('(-1,1),(2,-2)'::cube, 1);
SELECT cube_ll_coord('(-1,1),(2,-2)'::cube, 2);
SELECT cube_ll_coord('(-1,1),(2,-2)'::cube, 3);
SELECT cube_ll_coord('(1,2),(1,2)'::cube, 1);
SELECT cube_ll_coord('(1,2),(1,2)'::cube, 2);
SELECT cube_ll_coord('(1,2),(1,2)'::cube, 3);
SELECT cube_ll_coord('(42,137)'::cube, 1);
SELECT cube_ll_coord('(42,137)'::cube, 2);
SELECT cube_ll_coord('(42,137)'::cube, 3);

-- Test of cube_ur_coord function (retrieves UR coodinate values)
--
SELECT cube_ur_coord('(-1,1),(2,-2)'::cube, 1);
SELECT cube_ur_coord('(-1,1),(2,-2)'::cube, 2);
SELECT cube_ur_coord('(-1,1),(2,-2)'::cube, 3);
SELECT cube_ur_coord('(1,2),(1,2)'::cube, 1);
SELECT cube_ur_coord('(1,2),(1,2)'::cube, 2);
SELECT cube_ur_coord('(1,2),(1,2)'::cube, 3);
SELECT cube_ur_coord('(42,137)'::cube, 1);
SELECT cube_ur_coord('(42,137)'::cube, 2);
SELECT cube_ur_coord('(42,137)'::cube, 3);

-- Test of cube_is_point
--
SELECT cube_is_point('(0)'::cube);
SELECT cube_is_point('(0,1,2)'::cube);
SELECT cube_is_point('(0,1,2),(0,1,2)'::cube);
SELECT cube_is_point('(0,1,2),(-1,1,2)'::cube);
SELECT cube_is_point('(0,1,2),(0,-1,2)'::cube);
SELECT cube_is_point('(0,1,2),(0,1,-2)'::cube);

-- Test of cube_enlarge (enlarging and shrinking cubes)
--
SELECT cube_enlarge('(0)'::cube, 0, 0);
SELECT cube_enlarge('(0)'::cube, 0, 1);
SELECT cube_enlarge('(0)'::cube, 0, 2);
SELECT cube_enlarge('(2),(-2)'::cube, 0, 4);
SELECT cube_enlarge('(0)'::cube, 1, 0);
SELECT cube_enlarge('(0)'::cube, 1, 1);
SELECT cube_enlarge('(0)'::cube, 1, 2);
SELECT cube_enlarge('(2),(-2)'::cube, 1, 4);
SELECT cube_enlarge('(0)'::cube, -1, 0);
SELECT cube_enlarge('(0)'::cube, -1, 1);
SELECT cube_enlarge('(0)'::cube, -1, 2);
SELECT cube_enlarge('(2),(-2)'::cube, -1, 4);
SELECT cube_enlarge('(0,0,0)'::cube, 1, 0);
SELECT cube_enlarge('(0,0,0)'::cube, 1, 2);
SELECT cube_enlarge('(2,-2),(-3,7)'::cube, 1, 2);
SELECT cube_enlarge('(2,-2),(-3,7)'::cube, 3, 2);
SELECT cube_enlarge('(2,-2),(-3,7)'::cube, -1, 2);
SELECT cube_enlarge('(2,-2),(-3,7)'::cube, -3, 2);
SELECT cube_enlarge('(42,-23,-23),(42,23,23)'::cube, -23, 5);
SELECT cube_enlarge('(42,-23,-23),(42,23,23)'::cube, -24, 5);

-- Test of cube_union (MBR for two cubes)
--
SELECT cube_union('(1,2),(3,4)'::cube, '(5,6,7),(8,9,10)'::cube);
SELECT cube_union('(1,2)'::cube, '(4,2,0,0)'::cube);
SELECT cube_union('(1,2),(1,2)'::cube, '(4,2),(4,2)'::cube);
SELECT cube_union('(1,2),(1,2)'::cube, '(1,2),(1,2)'::cube);
SELECT cube_union('(1,2),(1,2)'::cube, '(1,2,0),(1,2,0)'::cube);

-- Test of cube_inter
--
SELECT cube_inter('(1,2),(10,11)'::cube, '(3,4), (16,15)'::cube); -- intersects
SELECT cube_inter('(1,2),(10,11)'::cube, '(3,4), (6,5)'::cube); -- includes
SELECT cube_inter('(1,2),(10,11)'::cube, '(13,14), (16,15)'::cube); -- no intersection
SELECT cube_inter('(1,2),(10,11)'::cube, '(3,14), (16,15)'::cube); -- no intersection, but one dimension intersects
SELECT cube_inter('(1,2),(10,11)'::cube, '(10,11), (16,15)'::cube); -- point intersection
SELECT cube_inter('(1,2,3)'::cube, '(1,2,3)'::cube); -- point args
SELECT cube_inter('(1,2,3)'::cube, '(5,6,3)'::cube); -- point args

-- Test of cube_size
--
SELECT cube_size('(4,8),(15,16)'::cube);
SELECT cube_size('(42,137)'::cube);

-- Test of distances
--
SELECT cube_distance('(1,1)'::cube, '(4,5)'::cube);
SELECT '(1,1)'::cube <-> '(4,5)'::cube as d_e;
SELECT distance_chebyshev('(1,1)'::cube, '(4,5)'::cube);
SELECT '(1,1)'::cube <=> '(4,5)'::cube as d_c;
SELECT distance_taxicab('(1,1)'::cube, '(4,5)'::cube);
SELECT '(1,1)'::cube <#> '(4,5)'::cube as d_t;
-- zero for overlapping
SELECT cube_distance('(2,2),(10,10)'::cube, '(0,0),(5,5)'::cube);
SELECT distance_chebyshev('(2,2),(10,10)'::cube, '(0,0),(5,5)'::cube);
SELECT distance_taxicab('(2,2),(10,10)'::cube, '(0,0),(5,5)'::cube);
-- coordinate access
SELECT cube(array[10,20,30], array[40,50,60])->1;
SELECT cube(array[40,50,60], array[10,20,30])->1;
SELECT cube(array[10,20,30], array[40,50,60])->6;
SELECT cube(array[10,20,30], array[40,50,60])->0;
SELECT cube(array[10,20,30], array[40,50,60])->7;
SELECT cube(array[10,20,30], array[40,50,60])->-1;
SELECT cube(array[10,20,30], array[40,50,60])->-6;
SELECT cube(array[10,20,30])->3;
SELECT cube(array[10,20,30])->6;
SELECT cube(array[10,20,30])->-6;
-- "normalized" coordinate access
SELECT cube(array[10,20,30], array[40,50,60])~>1;
SELECT cube(array[40,50,60], array[10,20,30])~>1;
SELECT cube(array[10,20,30], array[40,50,60])~>2;
SELECT cube(array[40,50,60], array[10,20,30])~>2;
SELECT cube(array[10,20,30], array[40,50,60])~>3;
SELECT cube(array[40,50,60], array[10,20,30])~>3;

SELECT cube(array[40,50,60], array[10,20,30])~>0;
SELECT cube(array[40,50,60], array[10,20,30])~>4;
SELECT cube(array[40,50,60], array[10,20,30])~>(-1);

-- Load some example data and build the index
--
CREATE TABLE test_cube (c cube);

\copy test_cube from 'data/test_cube.data'

CREATE INDEX test_cube_ix ON test_cube USING gist (c);
SELECT * FROM test_cube WHERE c && '(3000,1000),(0,0)' ORDER BY c;

-- Test sorting
SELECT * FROM test_cube WHERE c && '(3000,1000),(0,0)' GROUP BY c ORDER BY c;

-- kNN with index
SELECT *, c <-> '(100, 100),(500, 500)'::cube as dist FROM test_cube ORDER BY c <-> '(100, 100),(500, 500)'::cube LIMIT 5;
SELECT *, c <=> '(100, 100),(500, 500)'::cube as dist FROM test_cube ORDER BY c <=> '(100, 100),(500, 500)'::cube LIMIT 5;
SELECT *, c <#> '(100, 100),(500, 500)'::cube as dist FROM test_cube ORDER BY c <#> '(100, 100),(500, 500)'::cube LIMIT 5;

-- kNN-based sorting
SELECT * FROM test_cube ORDER BY c~>1 LIMIT 15; -- ascending by 1st coordinate of lower left corner
SELECT * FROM test_cube ORDER BY c~>4 LIMIT 15; -- ascending by 2nd coordinate or upper right corner
SELECT * FROM test_cube ORDER BY c~>1 DESC LIMIT 15; -- descending by 1st coordinate of lower left corner
SELECT * FROM test_cube ORDER BY c~>4 DESC LIMIT 15; -- descending by 2nd coordinate or upper right corner

-- same thing for index with points
CREATE TABLE test_point(c cube);
INSERT INTO test_point(SELECT cube(array[c->1,c->2,c->3,c->4]) FROM test_cube);
CREATE INDEX ON test_point USING gist(c);
SELECT * FROM test_point ORDER BY c~>1, c~>2 LIMIT 15; -- ascending by 1st then by 2nd coordinate
SELECT * FROM test_point ORDER BY c~>4 DESC LIMIT 15; -- descending by 1st coordinate
