/* contrib/test_parser/test_parser--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_parser" to load this file. \quit

ALTER EXTENSION test_parser ADD function testprs_start(internal,integer);
ALTER EXTENSION test_parser ADD function testprs_getlexeme(internal,internal,internal);
ALTER EXTENSION test_parser ADD function testprs_end(internal);
ALTER EXTENSION test_parser ADD function testprs_lextype(internal);
ALTER EXTENSION test_parser ADD text search parser testparser;
