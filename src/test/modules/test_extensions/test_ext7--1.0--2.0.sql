/* src/test/modules/test_extensions/test_ext7--1.0--2.0.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION test_ext7 UPDATE TO '2.0'" to load this file. \quit

-- drop some tables with serial columns
drop table ext7_table1;
drop table old_table1;
