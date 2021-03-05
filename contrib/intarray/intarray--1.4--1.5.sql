/* contrib/intarray/intarray--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION intarray UPDATE TO '1.5'" to load this file. \quit

-- Remove @ and ~
DROP OPERATOR @ (_int4, _int4);
DROP OPERATOR ~ (_int4, _int4);
