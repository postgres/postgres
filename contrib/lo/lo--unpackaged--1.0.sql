/* contrib/lo/lo--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION lo FROM unpackaged" to load this file. \quit

ALTER EXTENSION lo ADD domain lo;
ALTER EXTENSION lo ADD function lo_oid(lo);
ALTER EXTENSION lo ADD function lo_manage();
