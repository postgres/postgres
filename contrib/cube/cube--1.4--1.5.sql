/* contrib/cube/cube--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION cube UPDATE TO '1.5'" to load this file. \quit

-- Remove @ and ~
DROP OPERATOR @ (cube, cube);
DROP OPERATOR ~ (cube, cube);
