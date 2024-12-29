/* contrib/citext/citext--1.7--1.8.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION citext UPDATE TO '1.8'" to load this file. \quit

CREATE OR REPLACE FUNCTION regexp_match(string citext, pattern citext) RETURNS TEXT[]
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.regexp_match( $1::text, $2::text, 'i' );

CREATE OR REPLACE FUNCTION regexp_match(string citext, pattern citext, flags text) RETURNS TEXT[]
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.regexp_match( $1::text, $2::text, CASE WHEN pg_catalog.strpos($3, 'c') = 0 THEN  $3 || 'i' ELSE $3 END );

CREATE OR REPLACE FUNCTION regexp_matches(string citext, pattern citext) RETURNS SETOF TEXT[]
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE ROWS 1
RETURN pg_catalog.regexp_matches( $1::text, $2::text, 'i' );

CREATE OR REPLACE FUNCTION regexp_matches(string citext, pattern citext, flags text) RETURNS SETOF TEXT[]
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE ROWS 10
RETURN pg_catalog.regexp_matches( $1::text, $2::text, CASE WHEN pg_catalog.strpos($3, 'c') = 0 THEN  $3 || 'i' ELSE $3 END );

CREATE OR REPLACE FUNCTION regexp_replace(string citext, pattern citext, replacement text) returns TEXT
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.regexp_replace( $1::text, $2::text, $3, 'i');

CREATE OR REPLACE FUNCTION regexp_replace(string citext, pattern citext, replacement text, flags text) returns TEXT
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.regexp_replace( $1::text, $2::text, $3, CASE WHEN pg_catalog.strpos($4, 'c') = 0 THEN  $4 || 'i' ELSE $4 END);

CREATE OR REPLACE FUNCTION regexp_split_to_array(string citext, pattern citext) RETURNS TEXT[]
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.regexp_split_to_array( $1::text, $2::text, 'i' );

CREATE OR REPLACE FUNCTION regexp_split_to_array(string citext, pattern citext, flags text) RETURNS TEXT[]
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.regexp_split_to_array( $1::text, $2::text, CASE WHEN pg_catalog.strpos($3, 'c') = 0 THEN  $3 || 'i' ELSE $3 END );

CREATE OR REPLACE FUNCTION regexp_split_to_table(string citext, pattern citext) RETURNS SETOF TEXT
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.regexp_split_to_table( $1::text, $2::text, 'i' );

CREATE OR REPLACE FUNCTION regexp_split_to_table(string citext, pattern citext, flags text) RETURNS SETOF TEXT
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.regexp_split_to_table( $1::text, $2::text, CASE WHEN pg_catalog.strpos($3, 'c') = 0 THEN  $3 || 'i' ELSE $3 END );

CREATE OR REPLACE FUNCTION strpos( citext, citext ) RETURNS INT
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.strpos( pg_catalog.lower( $1::text ), pg_catalog.lower( $2::text ) );

CREATE OR REPLACE FUNCTION replace( citext, citext, citext ) RETURNS TEXT
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.regexp_replace( $1::text, pg_catalog.regexp_replace($2::text, '([^a-zA-Z_0-9])', E'\\\\\\1', 'g'), $3::text, 'gi' );

CREATE OR REPLACE FUNCTION split_part( citext, citext, int ) RETURNS TEXT
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN (pg_catalog.regexp_split_to_array( $1::text, pg_catalog.regexp_replace($2::text, '([^a-zA-Z_0-9])', E'\\\\\\1', 'g'), 'i'))[$3];

CREATE OR REPLACE FUNCTION translate( citext, citext, text ) RETURNS TEXT
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.translate( pg_catalog.translate( $1::text, pg_catalog.lower($2::text), $3), pg_catalog.upper($2::text), $3);
