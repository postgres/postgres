/* contrib/xml2/xml2--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION xml2 UPDATE TO '1.1'" to load this file. \quit

ALTER FUNCTION xml_valid(text) PARALLEL SAFE;
ALTER FUNCTION xml_encode_special_chars(text) PARALLEL SAFE;
ALTER FUNCTION xpath_string(text, text) PARALLEL SAFE;
ALTER FUNCTION xpath_nodeset(text, text, text, text) PARALLEL SAFE;
ALTER FUNCTION xpath_number(text, text) PARALLEL SAFE;
ALTER FUNCTION xpath_bool(text, text) PARALLEL SAFE;
ALTER FUNCTION xpath_list(text, text, text) PARALLEL SAFE;
ALTER FUNCTION xpath_list(text, text) PARALLEL SAFE;
ALTER FUNCTION xpath_nodeset(text, text) PARALLEL SAFE;
ALTER FUNCTION xpath_nodeset(text, text, text) PARALLEL SAFE;
ALTER FUNCTION xpath_table(text, text, text, text, text) PARALLEL SAFE;
ALTER FUNCTION xslt_process(text, text, text) PARALLEL SAFE;
ALTER FUNCTION xslt_process(text, text) PARALLEL SAFE;
