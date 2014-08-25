/* contrib/hstore/hstore--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION hstore UPDATE TO '1.1'" to load this file. \quit

ALTER EXTENSION hstore DROP OPERATOR => (text, text);
DROP OPERATOR => (text, text);
