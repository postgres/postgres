/* contrib/dict_xsyn/dict_xsyn--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION dict_xsyn" to load this file. \quit

ALTER EXTENSION dict_xsyn ADD function dxsyn_init(internal);
ALTER EXTENSION dict_xsyn ADD function dxsyn_lexize(internal,internal,internal,internal);
ALTER EXTENSION dict_xsyn ADD text search template xsyn_template;
ALTER EXTENSION dict_xsyn ADD text search dictionary xsyn;
