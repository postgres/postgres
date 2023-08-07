/* src/test/modules/test_extensions/test_ext_extschema--1.0.sql */
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_extschema" to load this file. \quit

SELECT 1 AS @extschema@;
