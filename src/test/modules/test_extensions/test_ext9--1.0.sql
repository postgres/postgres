/* src/test/modules/test_extensions/test_ext9--1.0.sql */
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext9" to load this file. \quit

-- check handling of types as extension members
create type varbitrange as range (subtype = varbit);
create table sometable (f1 real, f2 real);
create type somecomposite as (f1 float8, f2 float8);
