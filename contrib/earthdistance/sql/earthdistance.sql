--
--  Test earth distance functions
--

--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of earthdistance.sql or cube.sql.
--
\set ECHO none
\i earthdistance.sql
\set ECHO all

--
-- Test getting the distance between two points using geo_distance.
--

select geo_distance('(0,0)'::point,'(0,0)'::point)::numeric(20,5);
select geo_distance('(0,0)'::point,'(180,0)'::point)::numeric(20,5);
select geo_distance('(0,0)'::point,'(0,90)'::point)::numeric(20,5);
select geo_distance('(0,0)'::point,'(90,0)'::point)::numeric(20,5);
select geo_distance('(0,0)'::point,'(1,0)'::point)::numeric(20,5);
select geo_distance('(0,0)'::point,'(0,1)'::point)::numeric(20,5);
select geo_distance('(0,30)'::point,'(1,30)'::point)::numeric(20,5);
select geo_distance('(0,30)'::point,'(0,31)'::point)::numeric(20,5);
select geo_distance('(0,60)'::point,'(1,60)'::point)::numeric(20,5);
select geo_distance('(0,60)'::point,'(0,61)'::point)::numeric(20,5);
select geo_distance('(87.6,41.8)'::point,'(106.7,35.1)'::point)::numeric(20,5);
select (geo_distance('(87.6,41.8)'::point,'(106.7,35.1)'::point)*5280.*12.*2.54/100.)::numeric(20,5);

--
-- Test getting the distance between two points using the <@> operator.
--

select ('(0,0)'::point <@> '(0,0)'::point)::numeric(20,5);
select ('(0,0)'::point <@> '(180,0)'::point)::numeric(20,5);
select ('(0,0)'::point <@> '(0,90)'::point)::numeric(20,5);
select ('(0,0)'::point <@> '(90,0)'::point)::numeric(20,5);
select ('(0,0)'::point <@> '(1,0)'::point)::numeric(20,5);
select ('(0,0)'::point <@> '(0,1)'::point)::numeric(20,5);
select ('(0,30)'::point <@> '(1,30)'::point)::numeric(20,5);
select ('(0,30)'::point <@> '(0,31)'::point)::numeric(20,5);
select ('(0,60)'::point <@> '(1,60)'::point)::numeric(20,5);
select ('(0,60)'::point <@> '(0,61)'::point)::numeric(20,5);
select ('(87.6,41.8)'::point <@> '(106.7,35.1)'::point)::numeric(20,5);
select (('(87.6,41.8)'::point <@> '(106.7,35.1)'::point)*5280.*12.*2.54/100.)::numeric(20,5);
