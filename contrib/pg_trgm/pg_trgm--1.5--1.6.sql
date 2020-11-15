/* contrib/pg_trgm/pg_trgm--1.5--1.6.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_trgm UPDATE TO '1.6'" to load this file. \quit

ALTER OPERATOR FAMILY gin_trgm_ops USING gin ADD
        OPERATOR        11       pg_catalog.= (text, text);

ALTER OPERATOR FAMILY gist_trgm_ops USING gist ADD
        OPERATOR        11       pg_catalog.= (text, text);
