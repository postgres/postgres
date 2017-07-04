/* src/test/modules/test_extensions/test_ext7--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext7" to load this file. \quit

-- link some existing serial-owning table to the extension
alter extension test_ext7 add table old_table1;
alter extension test_ext7 add sequence old_table1_col1_seq;

-- ordinary member tables with serial columns
create table ext7_table1 (col1 serial primary key);

create table ext7_table2 (col2 serial primary key);
