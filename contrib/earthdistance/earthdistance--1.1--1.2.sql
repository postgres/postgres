/* contrib/earthdistance/earthdistance--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION earthdistance UPDATE TO '1.2'" to load this file. \quit

CREATE OR REPLACE FUNCTION earth() RETURNS float8
LANGUAGE SQL IMMUTABLE PARALLEL SAFE
RETURN '6378168'::float8;

CREATE OR REPLACE FUNCTION sec_to_gc(float8)
RETURNS float8
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
RETURN CASE
  WHEN $1 < '0'::float8 THEN '0'::float8
  WHEN $1 / ('2'::float8 * earth()) > '1'::float8 THEN pi() * earth()
  ELSE '2'::float8 * earth() * asin($1 / ('2'::float8 * earth()))
END;

CREATE OR REPLACE FUNCTION gc_to_sec(float8)
RETURNS float8
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
RETURN CASE
  WHEN $1 < '0'::float8 THEN '0'::float8
  WHEN $1 / earth() > pi() THEN '2'::float8 * earth()
  ELSE '2'::float8 * earth() * sin($1 / ('2'::float8 * earth()))
END;

CREATE OR REPLACE FUNCTION ll_to_earth(float8, float8)
RETURNS earth
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
RETURN @extschema:cube@.cube(@extschema:cube@.cube(@extschema:cube@.cube(
    earth() * cos(radians($1)) * cos(radians($2))),
    earth() * cos(radians($1)) * sin(radians($2))),
    earth() * sin(radians($1)))::earth;

CREATE OR REPLACE FUNCTION latitude(earth)
RETURNS float8
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
RETURN CASE
  WHEN @extschema:cube@.cube_ll_coord($1, 3) / earth() < '-1'::float8 THEN '-90'::float8
  WHEN @extschema:cube@.cube_ll_coord($1, 3) / earth() > '1'::float8 THEN '90'::float8
  ELSE degrees(asin(@extschema:cube@.cube_ll_coord($1, 3) / earth()))
END;

CREATE OR REPLACE FUNCTION longitude(earth)
RETURNS float8
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
RETURN degrees(atan2(@extschema:cube@.cube_ll_coord($1, 2),
                     @extschema:cube@.cube_ll_coord($1, 1)));

CREATE OR REPLACE FUNCTION earth_distance(earth, earth)
RETURNS float8
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
RETURN sec_to_gc(@extschema:cube@.cube_distance($1, $2));

CREATE OR REPLACE FUNCTION earth_box(earth, float8)
RETURNS @extschema:cube@.cube
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
RETURN @extschema:cube@.cube_enlarge($1, gc_to_sec($2), 3);
