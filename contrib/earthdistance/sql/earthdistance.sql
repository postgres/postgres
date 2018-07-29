--
--  Test earthdistance extension
--
-- In this file we also do some testing of extension create/drop scenarios.
-- That's really exercising the core database's dependency logic, so ideally
-- we'd do it in the core regression tests, but we can't for lack of suitable
-- guaranteed-available extensions.  earthdistance is a good test case because
-- it has a dependency on the cube extension.
--

CREATE EXTENSION earthdistance;  -- fail, must install cube first
CREATE EXTENSION cube;
CREATE EXTENSION earthdistance;

--
-- The radius of the Earth we are using.
--

SELECT earth()::numeric(20,5);

--
-- Convert straight line distances to great circle distances.
--
SELECT (pi()*earth())::numeric(20,5);
SELECT sec_to_gc(0)::numeric(20,5);
SELECT sec_to_gc(2*earth())::numeric(20,5);
SELECT sec_to_gc(10*earth())::numeric(20,5);
SELECT sec_to_gc(-earth())::numeric(20,5);
SELECT sec_to_gc(1000)::numeric(20,5);
SELECT sec_to_gc(10000)::numeric(20,5);
SELECT sec_to_gc(100000)::numeric(20,5);
SELECT sec_to_gc(1000000)::numeric(20,5);

--
-- Convert great circle distances to straight line distances.
--

SELECT gc_to_sec(0)::numeric(20,5);
SELECT gc_to_sec(sec_to_gc(2*earth()))::numeric(20,5);
SELECT gc_to_sec(10*earth())::numeric(20,5);
SELECT gc_to_sec(pi()*earth())::numeric(20,5);
SELECT gc_to_sec(-1000)::numeric(20,5);
SELECT gc_to_sec(1000)::numeric(20,5);
SELECT gc_to_sec(10000)::numeric(20,5);
SELECT gc_to_sec(100000)::numeric(20,5);
SELECT gc_to_sec(1000000)::numeric(20,5);

--
-- Set coordinates using latitude and longitude.
-- Extract each coordinate separately so we can round them.
--

SELECT cube_ll_coord(ll_to_earth(0,0),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,0),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,0),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(360,360),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(360,360),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(360,360),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(180,180),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(180,180),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(180,180),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(180,360),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(180,360),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(180,360),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(-180,-360),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-180,-360),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-180,-360),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(0,180),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,180),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,180),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(0,-180),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,-180),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,-180),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(90,0),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(90,0),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(90,0),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(90,180),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(90,180),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(90,180),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(-90,0),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-90,0),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-90,0),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(-90,180),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-90,180),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-90,180),3)::numeric(20,5);

--
-- Test getting the latitude of a location.
--

SELECT latitude(ll_to_earth(0,0))::numeric(20,10);
SELECT latitude(ll_to_earth(45,0))::numeric(20,10);
SELECT latitude(ll_to_earth(90,0))::numeric(20,10);
SELECT latitude(ll_to_earth(-45,0))::numeric(20,10);
SELECT latitude(ll_to_earth(-90,0))::numeric(20,10);
SELECT latitude(ll_to_earth(0,90))::numeric(20,10);
SELECT latitude(ll_to_earth(45,90))::numeric(20,10);
SELECT latitude(ll_to_earth(90,90))::numeric(20,10);
SELECT latitude(ll_to_earth(-45,90))::numeric(20,10);
SELECT latitude(ll_to_earth(-90,90))::numeric(20,10);
SELECT latitude(ll_to_earth(0,180))::numeric(20,10);
SELECT latitude(ll_to_earth(45,180))::numeric(20,10);
SELECT latitude(ll_to_earth(90,180))::numeric(20,10);
SELECT latitude(ll_to_earth(-45,180))::numeric(20,10);
SELECT latitude(ll_to_earth(-90,180))::numeric(20,10);
SELECT latitude(ll_to_earth(0,-90))::numeric(20,10);
SELECT latitude(ll_to_earth(45,-90))::numeric(20,10);
SELECT latitude(ll_to_earth(90,-90))::numeric(20,10);
SELECT latitude(ll_to_earth(-45,-90))::numeric(20,10);
SELECT latitude(ll_to_earth(-90,-90))::numeric(20,10);

--
-- Test getting the longitude of a location.
--

SELECT longitude(ll_to_earth(0,0))::numeric(20,10);
SELECT longitude(ll_to_earth(45,0))::numeric(20,10);
SELECT longitude(ll_to_earth(90,0))::numeric(20,10);
SELECT longitude(ll_to_earth(-45,0))::numeric(20,10);
SELECT longitude(ll_to_earth(-90,0))::numeric(20,10);
SELECT longitude(ll_to_earth(0,90))::numeric(20,10);
SELECT longitude(ll_to_earth(45,90))::numeric(20,10);
SELECT longitude(ll_to_earth(90,90))::numeric(20,10);
SELECT longitude(ll_to_earth(-45,90))::numeric(20,10);
SELECT longitude(ll_to_earth(-90,90))::numeric(20,10);
SELECT longitude(ll_to_earth(0,180))::numeric(20,10);
SELECT longitude(ll_to_earth(45,180))::numeric(20,10);
SELECT longitude(ll_to_earth(90,180))::numeric(20,10);
SELECT longitude(ll_to_earth(-45,180))::numeric(20,10);
SELECT longitude(ll_to_earth(-90,180))::numeric(20,10);
SELECT longitude(ll_to_earth(0,-90))::numeric(20,10);
SELECT longitude(ll_to_earth(45,-90))::numeric(20,10);
SELECT longitude(ll_to_earth(90,-90))::numeric(20,10);
SELECT longitude(ll_to_earth(-45,-90))::numeric(20,10);
SELECT longitude(ll_to_earth(-90,-90))::numeric(20,10);

--
-- For the distance tests the following is some real life data.
--
-- Chicago has a latitude of 41.8 and a longitude of 87.6.
-- Albuquerque has a latitude of 35.1 and a longitude of 106.7.
-- (Note that latitude and longitude are specified differently
-- in the cube based functions than for the point based functions.)
--

--
-- Test getting the distance between two points using earth_distance.
--

SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(0,0))::numeric(20,5);
SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(0,180))::numeric(20,5);
SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(90,0))::numeric(20,5);
SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(0,90))::numeric(20,5);
SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(0,1))::numeric(20,5);
SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(1,0))::numeric(20,5);
SELECT earth_distance(ll_to_earth(30,0),ll_to_earth(30,1))::numeric(20,5);
SELECT earth_distance(ll_to_earth(30,0),ll_to_earth(31,0))::numeric(20,5);
SELECT earth_distance(ll_to_earth(60,0),ll_to_earth(60,1))::numeric(20,5);
SELECT earth_distance(ll_to_earth(60,0),ll_to_earth(61,0))::numeric(20,5);
SELECT earth_distance(ll_to_earth(41.8,87.6),ll_to_earth(35.1,106.7))::numeric(20,5);
SELECT (earth_distance(ll_to_earth(41.8,87.6),ll_to_earth(35.1,106.7))*
      100./2.54/12./5280.)::numeric(20,5);

--
-- Test getting the distance between two points using geo_distance.
--

SELECT geo_distance('(0,0)'::point,'(0,0)'::point)::numeric(20,5);
SELECT geo_distance('(0,0)'::point,'(180,0)'::point)::numeric(20,5);
SELECT geo_distance('(0,0)'::point,'(0,90)'::point)::numeric(20,5);
SELECT geo_distance('(0,0)'::point,'(90,0)'::point)::numeric(20,5);
SELECT geo_distance('(0,0)'::point,'(1,0)'::point)::numeric(20,5);
SELECT geo_distance('(0,0)'::point,'(0,1)'::point)::numeric(20,5);
SELECT geo_distance('(0,30)'::point,'(1,30)'::point)::numeric(20,5);
SELECT geo_distance('(0,30)'::point,'(0,31)'::point)::numeric(20,5);
SELECT geo_distance('(0,60)'::point,'(1,60)'::point)::numeric(20,5);
SELECT geo_distance('(0,60)'::point,'(0,61)'::point)::numeric(20,5);
SELECT geo_distance('(87.6,41.8)'::point,'(106.7,35.1)'::point)::numeric(20,5);
SELECT (geo_distance('(87.6,41.8)'::point,'(106.7,35.1)'::point)*5280.*12.*2.54/100.)::numeric(20,5);

--
-- Test getting the distance between two points using the <@> operator.
--

SELECT ('(0,0)'::point <@> '(0,0)'::point)::numeric(20,5);
SELECT ('(0,0)'::point <@> '(180,0)'::point)::numeric(20,5);
SELECT ('(0,0)'::point <@> '(0,90)'::point)::numeric(20,5);
SELECT ('(0,0)'::point <@> '(90,0)'::point)::numeric(20,5);
SELECT ('(0,0)'::point <@> '(1,0)'::point)::numeric(20,5);
SELECT ('(0,0)'::point <@> '(0,1)'::point)::numeric(20,5);
SELECT ('(0,30)'::point <@> '(1,30)'::point)::numeric(20,5);
SELECT ('(0,30)'::point <@> '(0,31)'::point)::numeric(20,5);
SELECT ('(0,60)'::point <@> '(1,60)'::point)::numeric(20,5);
SELECT ('(0,60)'::point <@> '(0,61)'::point)::numeric(20,5);
SELECT ('(87.6,41.8)'::point <@> '(106.7,35.1)'::point)::numeric(20,5);
SELECT (('(87.6,41.8)'::point <@> '(106.7,35.1)'::point)*5280.*12.*2.54/100.)::numeric(20,5);

--
-- Test getting a bounding box around points.
--

SELECT cube_ll_coord(earth_box(ll_to_earth(0,0),112000),1)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),112000),2)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),112000),3)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),112000),1)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),112000),2)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),112000),3)::numeric(20,5);
SELECT cube_ll_coord(earth_box(ll_to_earth(0,0),pi()*earth()),1)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),pi()*earth()),2)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),pi()*earth()),3)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),pi()*earth()),1)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),pi()*earth()),2)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),pi()*earth()),3)::numeric(20,5);
SELECT cube_ll_coord(earth_box(ll_to_earth(0,0),10*earth()),1)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),10*earth()),2)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),10*earth()),3)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),10*earth()),1)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),10*earth()),2)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),10*earth()),3)::numeric(20,5);

--
-- Test for points that should be in bounding boxes.
--

SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,1))*1.00001) @>
       ll_to_earth(0,1);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.1))*1.00001) @>
       ll_to_earth(0,0.1);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.01))*1.00001) @>
       ll_to_earth(0,0.01);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.001))*1.00001) @>
       ll_to_earth(0,0.001);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.0001))*1.00001) @>
       ll_to_earth(0,0.0001);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0.0001,0.0001))*1.00001) @>
       ll_to_earth(0.0001,0.0001);
SELECT earth_box(ll_to_earth(45,45),
       earth_distance(ll_to_earth(45,45),ll_to_earth(45.0001,45.0001))*1.00001) @>
       ll_to_earth(45.0001,45.0001);
SELECT earth_box(ll_to_earth(90,180),
       earth_distance(ll_to_earth(90,180),ll_to_earth(90.0001,180.0001))*1.00001) @>
       ll_to_earth(90.0001,180.0001);

--
-- Test for points that shouldn't be in bounding boxes. Note that we need
-- to make points way outside, since some points close may be in the box
-- but further away than the distance we are testing.
--

SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,1))*.57735) @>
       ll_to_earth(0,1);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.1))*.57735) @>
       ll_to_earth(0,0.1);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.01))*.57735) @>
       ll_to_earth(0,0.01);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.001))*.57735) @>
       ll_to_earth(0,0.001);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.0001))*.57735) @>
       ll_to_earth(0,0.0001);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0.0001,0.0001))*.57735) @>
       ll_to_earth(0.0001,0.0001);
SELECT earth_box(ll_to_earth(45,45),
       earth_distance(ll_to_earth(45,45),ll_to_earth(45.0001,45.0001))*.57735) @>
       ll_to_earth(45.0001,45.0001);
SELECT earth_box(ll_to_earth(90,180),
       earth_distance(ll_to_earth(90,180),ll_to_earth(90.0001,180.0001))*.57735) @>
       ll_to_earth(90.0001,180.0001);

--
-- Test the recommended constraints.
--

SELECT cube_is_point(ll_to_earth(0,0));
SELECT cube_dim(ll_to_earth(0,0)) <= 3;
SELECT abs(cube_distance(ll_to_earth(0,0), '(0)'::cube) / earth() - 1) <
       '10e-12'::float8;
SELECT cube_is_point(ll_to_earth(30,60));
SELECT cube_dim(ll_to_earth(30,60)) <= 3;
SELECT abs(cube_distance(ll_to_earth(30,60), '(0)'::cube) / earth() - 1) <
       '10e-12'::float8;
SELECT cube_is_point(ll_to_earth(60,90));
SELECT cube_dim(ll_to_earth(60,90)) <= 3;
SELECT abs(cube_distance(ll_to_earth(60,90), '(0)'::cube) / earth() - 1) <
       '10e-12'::float8;
SELECT cube_is_point(ll_to_earth(-30,-90));
SELECT cube_dim(ll_to_earth(-30,-90)) <= 3;
SELECT abs(cube_distance(ll_to_earth(-30,-90), '(0)'::cube) / earth() - 1) <
       '10e-12'::float8;

--
-- Now we are going to test extension create/drop scenarios.
--

-- list what's installed
\dT

drop extension cube;  -- fail, earthdistance requires it

drop extension earthdistance;

drop type cube;  -- fail, extension cube requires it

-- list what's installed
\dT

create table foo (f1 cube, f2 int);

drop extension cube;  -- fail, foo.f1 requires it

drop table foo;

drop extension cube;

-- list what's installed
\dT
\df
\do

create schema c;

create extension cube with schema c;

-- list what's installed
\dT public.*
\df public.*
\do public.*
\dT c.*

create table foo (f1 c.cube, f2 int);

drop extension cube;  -- fail, foo.f1 requires it

drop schema c;  -- fail, cube requires it

drop extension cube cascade;

\d foo

-- list what's installed
\dT public.*
\df public.*
\do public.*
\dT c.*
\df c.*
\do c.*

drop schema c;
