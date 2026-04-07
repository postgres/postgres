/* contrib/pg_stash_advice/pg_stash_advice--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stash_advice" to load this file. \quit

CREATE FUNCTION pg_create_advice_stash(stash_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_create_advice_stash'
LANGUAGE C STRICT;

CREATE FUNCTION pg_drop_advice_stash(stash_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_drop_advice_stash'
LANGUAGE C STRICT;

CREATE FUNCTION pg_set_stashed_advice(stash_name text, query_id bigint,
									  advice_string text)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_set_stashed_advice'
LANGUAGE C;

CREATE FUNCTION pg_get_advice_stashes(
	OUT stash_name text,
	OUT num_entries bigint
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_advice_stashes'
LANGUAGE C STRICT;

CREATE FUNCTION pg_get_advice_stash_contents(
	INOUT stash_name text,
	OUT query_id bigint,
	OUT advice_string text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_advice_stash_contents'
LANGUAGE C;

CREATE FUNCTION pg_start_stash_advice_worker()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_start_stash_advice_worker'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION pg_create_advice_stash(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_drop_advice_stash(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_get_advice_stash_contents(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_get_advice_stashes() FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_set_stashed_advice(text, bigint, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_start_stash_advice_worker() FROM PUBLIC;
