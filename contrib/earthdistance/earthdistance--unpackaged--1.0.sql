/* contrib/earthdistance/earthdistance--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION earthdistance" to load this file. \quit

ALTER EXTENSION earthdistance ADD function earth();
ALTER EXTENSION earthdistance ADD type earth;
ALTER EXTENSION earthdistance ADD function sec_to_gc(double precision);
ALTER EXTENSION earthdistance ADD function gc_to_sec(double precision);
ALTER EXTENSION earthdistance ADD function ll_to_earth(double precision,double precision);
ALTER EXTENSION earthdistance ADD function latitude(earth);
ALTER EXTENSION earthdistance ADD function longitude(earth);
ALTER EXTENSION earthdistance ADD function earth_distance(earth,earth);
ALTER EXTENSION earthdistance ADD function earth_box(earth,double precision);
ALTER EXTENSION earthdistance ADD function geo_distance(point,point);
ALTER EXTENSION earthdistance ADD operator <@>(point,point);
