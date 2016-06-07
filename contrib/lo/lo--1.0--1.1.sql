/* contrib/lo/lo--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION lo UPDATE TO '1.1'" to load this file. \quit

ALTER FUNCTION lo_oid(lo) PARALLEL SAFE;
