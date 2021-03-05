/* contrib/seg/seg--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION seg UPDATE TO '1.4'" to load this file. \quit

-- Remove @ and ~
DROP OPERATOR @ (seg, seg);
DROP OPERATOR ~ (seg, seg);
