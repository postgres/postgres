/* contrib/earthdistance/earthdistance--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION earthdistance UPDATE TO '1.1'" to load this file. \quit

ALTER FUNCTION earth() PARALLEL SAFE;
ALTER FUNCTION sec_to_gc(float8) PARALLEL SAFE;
ALTER FUNCTION gc_to_sec(float8) PARALLEL SAFE;
ALTER FUNCTION ll_to_earth(float8, float8) PARALLEL SAFE;
ALTER FUNCTION latitude(earth) PARALLEL SAFE;
ALTER FUNCTION longitude(earth) PARALLEL SAFE;
ALTER FUNCTION earth_distance(earth, earth) PARALLEL SAFE;
ALTER FUNCTION earth_box(earth, float8) PARALLEL SAFE;
ALTER FUNCTION geo_distance(point, point) PARALLEL SAFE;
