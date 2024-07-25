/* contrib/citext/citext--1.6--1.7.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION citext UPDATE TO '1.7'" to load this file. \quit

-- add function argument names
CREATE OR REPLACE FUNCTION regexp_match(string citext, pattern citext) RETURNS TEXT[] AS $$
    SELECT pg_catalog.regexp_match( $1::pg_catalog.text, $2::pg_catalog.text, 'i' );
$$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION regexp_match(string citext, pattern citext, flags text) RETURNS TEXT[] AS $$
    SELECT pg_catalog.regexp_match( $1::pg_catalog.text, $2::pg_catalog.text, CASE WHEN pg_catalog.strpos($3, 'c') = 0 THEN  $3 || 'i' ELSE $3 END );
$$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION regexp_matches(string citext, pattern citext) RETURNS SETOF TEXT[] AS $$
    SELECT pg_catalog.regexp_matches( $1::pg_catalog.text, $2::pg_catalog.text, 'i' );
$$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE ROWS 1;

CREATE OR REPLACE FUNCTION regexp_matches(string citext, pattern citext, flags text) RETURNS SETOF TEXT[] AS $$
    SELECT pg_catalog.regexp_matches( $1::pg_catalog.text, $2::pg_catalog.text, CASE WHEN pg_catalog.strpos($3, 'c') = 0 THEN  $3 || 'i' ELSE $3 END );
$$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE ROWS 10;

CREATE OR REPLACE FUNCTION regexp_replace(string citext, pattern citext, replacement text) returns TEXT AS $$
    SELECT pg_catalog.regexp_replace( $1::pg_catalog.text, $2::pg_catalog.text, $3, 'i');
$$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION regexp_replace(string citext, pattern citext, replacement text, flags text) returns TEXT AS $$
    SELECT pg_catalog.regexp_replace( $1::pg_catalog.text, $2::pg_catalog.text, $3, CASE WHEN pg_catalog.strpos($4, 'c') = 0 THEN  $4 || 'i' ELSE $4 END);
$$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION regexp_split_to_array(string citext, pattern citext) RETURNS TEXT[] AS $$
    SELECT pg_catalog.regexp_split_to_array( $1::pg_catalog.text, $2::pg_catalog.text, 'i' );
$$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION regexp_split_to_array(string citext, pattern citext, flags text) RETURNS TEXT[] AS $$
    SELECT pg_catalog.regexp_split_to_array( $1::pg_catalog.text, $2::pg_catalog.text, CASE WHEN pg_catalog.strpos($3, 'c') = 0 THEN  $3 || 'i' ELSE $3 END );
$$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION regexp_split_to_table(string citext, pattern citext) RETURNS SETOF TEXT AS $$
    SELECT pg_catalog.regexp_split_to_table( $1::pg_catalog.text, $2::pg_catalog.text, 'i' );
$$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION regexp_split_to_table(string citext, pattern citext, flags text) RETURNS SETOF TEXT AS $$
    SELECT pg_catalog.regexp_split_to_table( $1::pg_catalog.text, $2::pg_catalog.text, CASE WHEN pg_catalog.strpos($3, 'c') = 0 THEN  $3 || 'i' ELSE $3 END );
$$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;
