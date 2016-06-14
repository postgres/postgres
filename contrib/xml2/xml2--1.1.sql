/* contrib/xml2/xml2--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION xml2" to load this file. \quit

--SQL for XML parser

-- deprecated old name for xml_is_well_formed
CREATE FUNCTION xml_valid(text) RETURNS bool
AS 'xml_is_well_formed'
LANGUAGE INTERNAL STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION xml_encode_special_chars(text) RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_string(text,text) RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_nodeset(text,text,text,text) RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_number(text,text) RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_bool(text,text) RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- List function

CREATE FUNCTION xpath_list(text,text,text) RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_list(text,text) RETURNS text
AS 'SELECT xpath_list($1,$2,'','')'
LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE;

-- Wrapper functions for nodeset where no tags needed

CREATE FUNCTION xpath_nodeset(text,text)
RETURNS text
AS 'SELECT xpath_nodeset($1,$2,'''','''')'
LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_nodeset(text,text,text)
RETURNS text
AS 'SELECT xpath_nodeset($1,$2,'''',$3)'
LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE;

-- Table function

CREATE FUNCTION xpath_table(text,text,text,text,text)
RETURNS setof record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE PARALLEL SAFE;

-- XSLT functions

CREATE FUNCTION xslt_process(text,text,text)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- the function checks for the correct argument count
CREATE FUNCTION xslt_process(text,text)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
