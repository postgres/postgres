/*
 * PostgreSQL System Functions
 *
 * Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * src/backend/catalog/system_functions.sql
 *
 * This file redefines certain built-in functions that it's impractical
 * to fully define in pg_proc.dat.  In most cases that's because they use
 * SQL-standard function bodies and/or default expressions.  The node
 * tree representations of those are too unreadable, platform-dependent,
 * and changeable to want to deal with them manually.  Hence, we put stub
 * definitions of such functions into pg_proc.dat and then replace them
 * here.  The stub definitions would be unnecessary were it not that we'd
 * like these functions to have stable OIDs, the same as other built-in
 * functions.
 *
 * This file also takes care of adjusting privileges for those functions
 * that should not have the default public-EXECUTE privileges.  (However,
 * a small number of functions that exist mainly to underlie system views
 * are dealt with in system_views.sql, instead.)
 *
 * Note: this file is read in single-user -j mode, which means that the
 * command terminator is semicolon-newline-newline; whenever the backend
 * sees that, it stops and executes what it's got.  If you write a lot of
 * statements without empty lines between, they'll all get quoted to you
 * in any error message about one of them, so don't do that.  Also, you
 * cannot write a semicolon immediately followed by an empty line in a
 * string literal (including a function body!) or a multiline comment.
 */


CREATE FUNCTION ts_debug(IN config regconfig, IN document text,
    OUT alias text,
    OUT description text,
    OUT token text,
    OUT dictionaries regdictionary[],
    OUT dictionary regdictionary,
    OUT lexemes text[])
RETURNS SETOF record AS
$$
SELECT
    tt.alias AS alias,
    tt.description AS description,
    parse.token AS token,
    ARRAY ( SELECT m.mapdict::pg_catalog.regdictionary
            FROM pg_catalog.pg_ts_config_map AS m
            WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
            ORDER BY m.mapseqno )
    AS dictionaries,
    ( SELECT mapdict::pg_catalog.regdictionary
      FROM pg_catalog.pg_ts_config_map AS m
      WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
      ORDER BY pg_catalog.ts_lexize(mapdict, parse.token) IS NULL, m.mapseqno
      LIMIT 1
    ) AS dictionary,
    ( SELECT pg_catalog.ts_lexize(mapdict, parse.token)
      FROM pg_catalog.pg_ts_config_map AS m
      WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
      ORDER BY pg_catalog.ts_lexize(mapdict, parse.token) IS NULL, m.mapseqno
      LIMIT 1
    ) AS lexemes
FROM pg_catalog.ts_parse(
        (SELECT cfgparser FROM pg_catalog.pg_ts_config WHERE oid = $1 ), $2
    ) AS parse,
     pg_catalog.ts_token_type(
        (SELECT cfgparser FROM pg_catalog.pg_ts_config WHERE oid = $1 )
    ) AS tt
WHERE tt.tokid = parse.tokid
$$
LANGUAGE SQL STRICT STABLE PARALLEL SAFE;

COMMENT ON FUNCTION ts_debug(regconfig,text) IS
    'debug function for text search configuration';

CREATE FUNCTION ts_debug(IN document text,
    OUT alias text,
    OUT description text,
    OUT token text,
    OUT dictionaries regdictionary[],
    OUT dictionary regdictionary,
    OUT lexemes text[])
RETURNS SETOF record AS
$$
    SELECT * FROM pg_catalog.ts_debug( pg_catalog.get_current_ts_config(), $1);
$$
LANGUAGE SQL STRICT STABLE PARALLEL SAFE;

COMMENT ON FUNCTION ts_debug(text) IS
    'debug function for current text search configuration';


CREATE OR REPLACE FUNCTION
  pg_start_backup(label text, fast boolean DEFAULT false, exclusive boolean DEFAULT true)
  RETURNS pg_lsn STRICT VOLATILE LANGUAGE internal AS 'pg_start_backup'
  PARALLEL RESTRICTED;

CREATE OR REPLACE FUNCTION pg_stop_backup (
        exclusive boolean, wait_for_archive boolean DEFAULT true,
        OUT lsn pg_lsn, OUT labelfile text, OUT spcmapfile text)
  RETURNS SETOF record STRICT VOLATILE LANGUAGE internal as 'pg_stop_backup_v2'
  PARALLEL RESTRICTED;

CREATE OR REPLACE FUNCTION
  pg_promote(wait boolean DEFAULT true, wait_seconds integer DEFAULT 60)
  RETURNS boolean STRICT VOLATILE LANGUAGE INTERNAL AS 'pg_promote'
  PARALLEL SAFE;

CREATE OR REPLACE FUNCTION
  pg_terminate_backend(pid integer, timeout int8 DEFAULT 0)
  RETURNS boolean STRICT VOLATILE LANGUAGE INTERNAL AS 'pg_terminate_backend'
  PARALLEL SAFE;

CREATE OR REPLACE FUNCTION
  pg_wait_for_backend_termination(pid integer, timeout int8 DEFAULT 5000)
  RETURNS boolean STRICT VOLATILE LANGUAGE INTERNAL AS 'pg_wait_for_backend_termination'
  PARALLEL SAFE;

-- legacy definition for compatibility with 9.3
CREATE OR REPLACE FUNCTION
  json_populate_record(base anyelement, from_json json, use_json_as_text boolean DEFAULT false)
  RETURNS anyelement LANGUAGE internal STABLE AS 'json_populate_record' PARALLEL SAFE;

-- legacy definition for compatibility with 9.3
CREATE OR REPLACE FUNCTION
  json_populate_recordset(base anyelement, from_json json, use_json_as_text boolean DEFAULT false)
  RETURNS SETOF anyelement LANGUAGE internal STABLE ROWS 100  AS 'json_populate_recordset' PARALLEL SAFE;

CREATE OR REPLACE FUNCTION pg_logical_slot_get_changes(
    IN slot_name name, IN upto_lsn pg_lsn, IN upto_nchanges int, VARIADIC options text[] DEFAULT '{}',
    OUT lsn pg_lsn, OUT xid xid, OUT data text)
RETURNS SETOF RECORD
LANGUAGE INTERNAL
VOLATILE ROWS 1000 COST 1000
AS 'pg_logical_slot_get_changes';

CREATE OR REPLACE FUNCTION pg_logical_slot_peek_changes(
    IN slot_name name, IN upto_lsn pg_lsn, IN upto_nchanges int, VARIADIC options text[] DEFAULT '{}',
    OUT lsn pg_lsn, OUT xid xid, OUT data text)
RETURNS SETOF RECORD
LANGUAGE INTERNAL
VOLATILE ROWS 1000 COST 1000
AS 'pg_logical_slot_peek_changes';

CREATE OR REPLACE FUNCTION pg_logical_slot_get_binary_changes(
    IN slot_name name, IN upto_lsn pg_lsn, IN upto_nchanges int, VARIADIC options text[] DEFAULT '{}',
    OUT lsn pg_lsn, OUT xid xid, OUT data bytea)
RETURNS SETOF RECORD
LANGUAGE INTERNAL
VOLATILE ROWS 1000 COST 1000
AS 'pg_logical_slot_get_binary_changes';

CREATE OR REPLACE FUNCTION pg_logical_slot_peek_binary_changes(
    IN slot_name name, IN upto_lsn pg_lsn, IN upto_nchanges int, VARIADIC options text[] DEFAULT '{}',
    OUT lsn pg_lsn, OUT xid xid, OUT data bytea)
RETURNS SETOF RECORD
LANGUAGE INTERNAL
VOLATILE ROWS 1000 COST 1000
AS 'pg_logical_slot_peek_binary_changes';

CREATE OR REPLACE FUNCTION pg_create_physical_replication_slot(
    IN slot_name name, IN immediately_reserve boolean DEFAULT false,
    IN temporary boolean DEFAULT false,
    OUT slot_name name, OUT lsn pg_lsn)
RETURNS RECORD
LANGUAGE INTERNAL
STRICT VOLATILE
AS 'pg_create_physical_replication_slot';

CREATE OR REPLACE FUNCTION pg_create_logical_replication_slot(
    IN slot_name name, IN plugin name,
    IN temporary boolean DEFAULT false,
    IN twophase boolean DEFAULT false,
    OUT slot_name name, OUT lsn pg_lsn)
RETURNS RECORD
LANGUAGE INTERNAL
STRICT VOLATILE
AS 'pg_create_logical_replication_slot';

CREATE OR REPLACE FUNCTION
  make_interval(years int4 DEFAULT 0, months int4 DEFAULT 0, weeks int4 DEFAULT 0,
                days int4 DEFAULT 0, hours int4 DEFAULT 0, mins int4 DEFAULT 0,
                secs double precision DEFAULT 0.0)
RETURNS interval
LANGUAGE INTERNAL
STRICT IMMUTABLE PARALLEL SAFE
AS 'make_interval';

CREATE OR REPLACE FUNCTION
  jsonb_set(jsonb_in jsonb, path text[] , replacement jsonb,
            create_if_missing boolean DEFAULT true)
RETURNS jsonb
LANGUAGE INTERNAL
STRICT IMMUTABLE PARALLEL SAFE
AS 'jsonb_set';

CREATE OR REPLACE FUNCTION
  jsonb_set_lax(jsonb_in jsonb, path text[] , replacement jsonb,
            create_if_missing boolean DEFAULT true,
            null_value_treatment text DEFAULT 'use_json_null')
RETURNS jsonb
LANGUAGE INTERNAL
CALLED ON NULL INPUT IMMUTABLE PARALLEL SAFE
AS 'jsonb_set_lax';

CREATE OR REPLACE FUNCTION
  parse_ident(str text, strict boolean DEFAULT true)
RETURNS text[]
LANGUAGE INTERNAL
STRICT IMMUTABLE PARALLEL SAFE
AS 'parse_ident';

CREATE OR REPLACE FUNCTION
  jsonb_insert(jsonb_in jsonb, path text[] , replacement jsonb,
            insert_after boolean DEFAULT false)
RETURNS jsonb
LANGUAGE INTERNAL
STRICT IMMUTABLE PARALLEL SAFE
AS 'jsonb_insert';

CREATE OR REPLACE FUNCTION
  jsonb_path_exists(target jsonb, path jsonpath, vars jsonb DEFAULT '{}',
                    silent boolean DEFAULT false)
RETURNS boolean
LANGUAGE INTERNAL
STRICT IMMUTABLE PARALLEL SAFE
AS 'jsonb_path_exists';

CREATE OR REPLACE FUNCTION
  jsonb_path_match(target jsonb, path jsonpath, vars jsonb DEFAULT '{}',
                   silent boolean DEFAULT false)
RETURNS boolean
LANGUAGE INTERNAL
STRICT IMMUTABLE PARALLEL SAFE
AS 'jsonb_path_match';

CREATE OR REPLACE FUNCTION
  jsonb_path_query(target jsonb, path jsonpath, vars jsonb DEFAULT '{}',
                   silent boolean DEFAULT false)
RETURNS SETOF jsonb
LANGUAGE INTERNAL
STRICT IMMUTABLE PARALLEL SAFE
AS 'jsonb_path_query';

CREATE OR REPLACE FUNCTION
  jsonb_path_query_array(target jsonb, path jsonpath, vars jsonb DEFAULT '{}',
                         silent boolean DEFAULT false)
RETURNS jsonb
LANGUAGE INTERNAL
STRICT IMMUTABLE PARALLEL SAFE
AS 'jsonb_path_query_array';

CREATE OR REPLACE FUNCTION
  jsonb_path_query_first(target jsonb, path jsonpath, vars jsonb DEFAULT '{}',
                         silent boolean DEFAULT false)
RETURNS jsonb
LANGUAGE INTERNAL
STRICT IMMUTABLE PARALLEL SAFE
AS 'jsonb_path_query_first';

CREATE OR REPLACE FUNCTION
  jsonb_path_exists_tz(target jsonb, path jsonpath, vars jsonb DEFAULT '{}',
                    silent boolean DEFAULT false)
RETURNS boolean
LANGUAGE INTERNAL
STRICT STABLE PARALLEL SAFE
AS 'jsonb_path_exists_tz';

CREATE OR REPLACE FUNCTION
  jsonb_path_match_tz(target jsonb, path jsonpath, vars jsonb DEFAULT '{}',
                   silent boolean DEFAULT false)
RETURNS boolean
LANGUAGE INTERNAL
STRICT STABLE PARALLEL SAFE
AS 'jsonb_path_match_tz';

CREATE OR REPLACE FUNCTION
  jsonb_path_query_tz(target jsonb, path jsonpath, vars jsonb DEFAULT '{}',
                   silent boolean DEFAULT false)
RETURNS SETOF jsonb
LANGUAGE INTERNAL
STRICT STABLE PARALLEL SAFE
AS 'jsonb_path_query_tz';

CREATE OR REPLACE FUNCTION
  jsonb_path_query_array_tz(target jsonb, path jsonpath, vars jsonb DEFAULT '{}',
                         silent boolean DEFAULT false)
RETURNS jsonb
LANGUAGE INTERNAL
STRICT STABLE PARALLEL SAFE
AS 'jsonb_path_query_array_tz';

CREATE OR REPLACE FUNCTION
  jsonb_path_query_first_tz(target jsonb, path jsonpath, vars jsonb DEFAULT '{}',
                         silent boolean DEFAULT false)
RETURNS jsonb
LANGUAGE INTERNAL
STRICT STABLE PARALLEL SAFE
AS 'jsonb_path_query_first_tz';

-- default normalization form is NFC, per SQL standard
CREATE OR REPLACE FUNCTION
  "normalize"(text, text DEFAULT 'NFC')
RETURNS text
LANGUAGE internal
STRICT IMMUTABLE PARALLEL SAFE
AS 'unicode_normalize_func';

CREATE OR REPLACE FUNCTION
  is_normalized(text, text DEFAULT 'NFC')
RETURNS boolean
LANGUAGE internal
STRICT IMMUTABLE PARALLEL SAFE
AS 'unicode_is_normalized';

--
-- The default permissions for functions mean that anyone can execute them.
-- A number of functions shouldn't be executable by just anyone, but rather
-- than use explicit 'superuser()' checks in those functions, we use the GRANT
-- system to REVOKE access to those functions at initdb time.  Administrators
-- can later change who can access these functions, or leave them as only
-- available to superuser / cluster owner, if they choose.
--
REVOKE EXECUTE ON FUNCTION pg_start_backup(text, boolean, boolean) FROM public;
REVOKE EXECUTE ON FUNCTION pg_stop_backup() FROM public;
REVOKE EXECUTE ON FUNCTION pg_stop_backup(boolean, boolean) FROM public;
REVOKE EXECUTE ON FUNCTION pg_create_restore_point(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_switch_wal() FROM public;
REVOKE EXECUTE ON FUNCTION pg_wal_replay_pause() FROM public;
REVOKE EXECUTE ON FUNCTION pg_wal_replay_resume() FROM public;
REVOKE EXECUTE ON FUNCTION pg_rotate_logfile() FROM public;
REVOKE EXECUTE ON FUNCTION pg_reload_conf() FROM public;
REVOKE EXECUTE ON FUNCTION pg_current_logfile() FROM public;
REVOKE EXECUTE ON FUNCTION pg_current_logfile(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_promote(boolean, integer) FROM public;

REVOKE EXECUTE ON FUNCTION pg_stat_reset() FROM public;
REVOKE EXECUTE ON FUNCTION pg_stat_reset_shared(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_stat_reset_slru(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_stat_reset_single_table_counters(oid) FROM public;
REVOKE EXECUTE ON FUNCTION pg_stat_reset_single_function_counters(oid) FROM public;
REVOKE EXECUTE ON FUNCTION pg_stat_reset_replication_slot(text) FROM public;

REVOKE EXECUTE ON FUNCTION lo_import(text) FROM public;
REVOKE EXECUTE ON FUNCTION lo_import(text, oid) FROM public;
REVOKE EXECUTE ON FUNCTION lo_export(oid, text) FROM public;

REVOKE EXECUTE ON FUNCTION pg_ls_logdir() FROM public;
REVOKE EXECUTE ON FUNCTION pg_ls_waldir() FROM public;
REVOKE EXECUTE ON FUNCTION pg_ls_archive_statusdir() FROM public;
REVOKE EXECUTE ON FUNCTION pg_ls_tmpdir() FROM public;
REVOKE EXECUTE ON FUNCTION pg_ls_tmpdir(oid) FROM public;

REVOKE EXECUTE ON FUNCTION pg_read_file(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_read_file(text,bigint,bigint) FROM public;
REVOKE EXECUTE ON FUNCTION pg_read_file(text,bigint,bigint,boolean) FROM public;

REVOKE EXECUTE ON FUNCTION pg_read_binary_file(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_read_binary_file(text,bigint,bigint) FROM public;
REVOKE EXECUTE ON FUNCTION pg_read_binary_file(text,bigint,bigint,boolean) FROM public;

REVOKE EXECUTE ON FUNCTION pg_replication_origin_advance(text, pg_lsn) FROM public;
REVOKE EXECUTE ON FUNCTION pg_replication_origin_create(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_replication_origin_drop(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_replication_origin_oid(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_replication_origin_progress(text, boolean) FROM public;
REVOKE EXECUTE ON FUNCTION pg_replication_origin_session_is_setup() FROM public;
REVOKE EXECUTE ON FUNCTION pg_replication_origin_session_progress(boolean) FROM public;
REVOKE EXECUTE ON FUNCTION pg_replication_origin_session_reset() FROM public;
REVOKE EXECUTE ON FUNCTION pg_replication_origin_session_setup(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_replication_origin_xact_reset() FROM public;
REVOKE EXECUTE ON FUNCTION pg_replication_origin_xact_setup(pg_lsn, timestamp with time zone) FROM public;
REVOKE EXECUTE ON FUNCTION pg_show_replication_origin_status() FROM public;

REVOKE EXECUTE ON FUNCTION pg_stat_file(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_stat_file(text,boolean) FROM public;

REVOKE EXECUTE ON FUNCTION pg_ls_dir(text) FROM public;
REVOKE EXECUTE ON FUNCTION pg_ls_dir(text,boolean,boolean) FROM public;

--
-- We also set up some things as accessible to standard roles.
--
GRANT EXECUTE ON FUNCTION pg_ls_logdir() TO pg_monitor;
GRANT EXECUTE ON FUNCTION pg_ls_waldir() TO pg_monitor;
GRANT EXECUTE ON FUNCTION pg_ls_archive_statusdir() TO pg_monitor;
GRANT EXECUTE ON FUNCTION pg_ls_tmpdir() TO pg_monitor;
GRANT EXECUTE ON FUNCTION pg_ls_tmpdir(oid) TO pg_monitor;

GRANT pg_read_all_settings TO pg_monitor;
GRANT pg_read_all_stats TO pg_monitor;
GRANT pg_stat_scan_tables TO pg_monitor;
