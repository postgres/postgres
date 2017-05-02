/* src/test/modules/test_extensions/test_ext3--1.0.sql */
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext3" to load this file. \quit

CREATE TABLE test_ext3_table (col_old INT);

ALTER TABLE test_ext3_table RENAME col_old TO col_new;

UPDATE test_ext3_table SET col_new = 0;
