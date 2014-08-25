/* contrib/spi/timetravel--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION timetravel FROM unpackaged" to load this file. \quit

ALTER EXTENSION timetravel ADD function timetravel();
ALTER EXTENSION timetravel ADD function set_timetravel(name,integer);
ALTER EXTENSION timetravel ADD function get_timetravel(name);
