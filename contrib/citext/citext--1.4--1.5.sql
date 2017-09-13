/* contrib/citext/citext--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION citext UPDATE TO '1.5'" to load this file. \quit

ALTER OPERATOR <= (citext, citext) SET (
    RESTRICT   = scalarlesel,
    JOIN       = scalarlejoinsel
);

ALTER OPERATOR >= (citext, citext) SET (
    RESTRICT   = scalargesel,
    JOIN       = scalargejoinsel
);
