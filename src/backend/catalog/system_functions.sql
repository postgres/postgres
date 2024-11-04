/*
 * PostgreSQL System Functions
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/catalog/system_functions.sql
 *
 * This file redefines certain built-in functions that are impractical
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


CREATE OR REPLACE FUNCTION lpad(text, integer)
 RETURNS text
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN lpad($1, $2, ' ');

CREATE OR REPLACE FUNCTION rpad(text, integer)
 RETURNS text
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN rpad($1, $2, ' ');

CREATE OR REPLACE FUNCTION "substring"(text, text, text)
 RETURNS text
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN substring($1, similar_to_escape($2, $3));

CREATE OR REPLACE FUNCTION bit_length(bit)
 RETURNS integer
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN length($1);

CREATE OR REPLACE FUNCTION bit_length(bytea)
 RETURNS integer
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN octet_length($1) * 8;

CREATE OR REPLACE FUNCTION bit_length(text)
 RETURNS integer
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN octet_length($1) * 8;

CREATE OR REPLACE FUNCTION
 random_normal(mean float8 DEFAULT 0, stddev float8 DEFAULT 1)
 RETURNS float8
 LANGUAGE internal
 VOLATILE PARALLEL RESTRICTED STRICT COST 1
AS 'drandom_normal';

CREATE OR REPLACE FUNCTION log(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN log(10, $1);

CREATE OR REPLACE FUNCTION log10(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN log(10, $1);

CREATE OR REPLACE FUNCTION round(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN round($1, 0);

CREATE OR REPLACE FUNCTION trunc(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN trunc($1, 0);

CREATE OR REPLACE FUNCTION numeric_pl_pg_lsn(numeric, pg_lsn)
 RETURNS pg_lsn
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION path_contain_pt(path, point)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN on_ppath($2, $1);

CREATE OR REPLACE FUNCTION polygon(circle)
 RETURNS polygon
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN polygon(12, $1);

CREATE OR REPLACE FUNCTION age(timestamptz)
 RETURNS interval
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN age(cast(current_date as timestamptz), $1);

CREATE OR REPLACE FUNCTION age(timestamp)
 RETURNS interval
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN age(cast(current_date as timestamp), $1);

CREATE OR REPLACE FUNCTION date_part(text, date)
 RETURNS double precision
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN date_part($1, cast($2 as timestamp));

CREATE OR REPLACE FUNCTION timestamptz(date, time)
 RETURNS timestamptz
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN cast(($1 + $2) as timestamptz);

CREATE OR REPLACE FUNCTION timedate_pl(time, date)
 RETURNS timestamp
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION timetzdate_pl(timetz, date)
 RETURNS timestamptz
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_time(interval, time)
 RETURNS time
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_date(interval, date)
 RETURNS timestamp
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_timetz(interval, timetz)
 RETURNS timetz
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_timestamp(interval, timestamp)
 RETURNS timestamp
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_timestamptz(interval, timestamptz)
 RETURNS timestamptz
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION integer_pl_date(integer, date)
 RETURNS date
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION "overlaps"(timestamptz, timestamptz,
  timestamptz, interval)
 RETURNS boolean
 LANGUAGE sql
 STABLE PARALLEL SAFE COST 1
RETURN ($1, $2) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(timestamptz, interval,
  timestamptz, interval)
 RETURNS boolean
 LANGUAGE sql
 STABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(timestamptz, interval,
  timestamptz, timestamptz)
 RETURNS boolean
 LANGUAGE sql
 STABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, $4);

CREATE OR REPLACE FUNCTION "overlaps"(timestamp, timestamp,
  timestamp, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, $2) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(timestamp, interval,
  timestamp, timestamp)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, $4);

CREATE OR REPLACE FUNCTION "overlaps"(timestamp, interval,
  timestamp, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(time, interval,
  time, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(time, time,
  time, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, $2) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(time, interval,
  time, time)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, $4);

CREATE OR REPLACE FUNCTION int8pl_inet(bigint, inet)
 RETURNS inet
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION xpath(text, xml)
 RETURNS xml[]
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN xpath($1, $2, '{}'::text[]);

CREATE OR REPLACE FUNCTION xpath_exists(text, xml)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN xpath_exists($1, $2, '{}'::text[]);

CREATE OR REPLACE FUNCTION pg_sleep_for(interval)
 RETURNS void
 LANGUAGE sql
 PARALLEL SAFE STRICT COST 1
RETURN pg_sleep(extract(epoch from clock_timestamp() + $1) -
                extract(epoch from clock_timestamp()));

CREATE OR REPLACE FUNCTION pg_sleep_until(timestamptz)
 RETURNS void
 LANGUAGE sql
 PARALLEL SAFE STRICT COST 1
RETURN pg_sleep(extract(epoch from $1) -
                extract(epoch from clock_timestamp()));

CREATE OR REPLACE FUNCTION pg_relation_size(regclass)
 RETURNS bigint
 LANGUAGE sql
 PARALLEL SAFE STRICT COST 1
RETURN pg_relation_size($1, 'main');

CREATE OR REPLACE FUNCTION obj_description(oid, name)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_description
  where objoid = $1 and
    classoid = (select oid from pg_class where relname = $2 and
                relnamespace = 'pg_catalog'::regnamespace) and
    objsubid = 0;
END;

CREATE OR REPLACE FUNCTION shobj_description(oid, name)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_shdescription
  where objoid = $1 and
    classoid = (select oid from pg_class where relname = $2 and
                relnamespace = 'pg_catalog'::regnamespace);
END;

CREATE OR REPLACE FUNCTION obj_description(oid)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_description where objoid = $1 and objsubid = 0;
END;

CREATE OR REPLACE FUNCTION col_description(oid, integer)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_description
  where objoid = $1 and classoid = 'pg_class'::regclass and objsubid = $2;
END;

CREATE OR REPLACE FUNCTION ts_debug(config regconfig, document text,
    OUT alias text,
    OUT description text,
    OUT token text,
    OUT dictionaries regdictionary[],
    OUT dictionary regdictionary,
    OUT lexemes text[])
 RETURNS SETOF record
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select
    tt.alias AS alias,
    tt.description AS description,
    parse.token AS token,
    ARRAY ( SELECT m.mapdict::regdictionary
            FROM pg_ts_config_map AS m
            WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
            ORDER BY m.mapseqno )
    AS dictionaries,
    ( SELECT mapdict::regdictionary
      FROM pg_ts_config_map AS m
      WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
      ORDER BY ts_lexize(mapdict, parse.token) IS NULL, m.mapseqno
      LIMIT 1
    ) AS dictionary,
    ( SELECT ts_lexize(mapdict, parse.token)
      FROM pg_ts_config_map AS m
      WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
      ORDER BY ts_lexize(mapdict, parse.token) IS NULL, m.mapseqno
      LIMIT 1
    ) AS lexemes
FROM ts_parse(
        (SELECT cfgparser FROM pg_ts_config WHERE oid = $1 ), $2
    ) AS parse,
     ts_token_type(
        (SELECT cfgparser FROM pg_ts_config WHERE oid = $1 )
    ) AS tt
WHERE tt.tokid = parse.tokid;
END;

CREATE OR REPLACE FUNCTION ts_debug(document text,
    OUT alias text,
    OUT description text,
    OUT token text,
    OUT dictionaries regdictionary[],
    OUT dictionary regdictionary,
    OUT lexemes text[])
 RETURNS SETOF record
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
    SELECT * FROM ts_debug(get_current_ts_config(), $1);
END;

CREATE OR REPLACE FUNCTION
  pg_backup_start(label text, fast boolean DEFAULT false)
  RETURNS pg_lsn STRICT VOLATILE LANGUAGE internal AS 'pg_backup_start'
  PARALLEL RESTRICTED;

CREATE OR REPLACE FUNCTION pg_backup_stop (
        wait_for_archive boolean DEFAULT true, OUT lsn pg_lsn,
        OUT labelfile text, OUT spcmapfile text)
  RETURNS record STRICT VOLATILE LANGUAGE internal as 'pg_backup_stop'
  PARALLEL RESTRICTED;

CREATE OR REPLACE FUNCTION
  pg_promote(wait boolean DEFAULT true, wait_seconds integer DEFAULT 60)
  RETURNS boolean STRICT VOLATILE LANGUAGE INTERNAL AS 'pg_promote'
  PARALLEL SAFE;

CREATE OR REPLACE FUNCTION
  pg_terminate_backend(pid integer, timeout int8 DEFAULT 0)
  RETURNS boolean STRICT VOLATILE LANGUAGE INTERNAL AS 'pg_terminate_backend'
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

CREATE OR REPLACE FUNCTION pg_logical_emit_message(
    transactional boolean,
    prefix text,
    message text,
    flush boolean DEFAULT false)
RETURNS pg_lsn
LANGUAGE INTERNAL
STRICT VOLATILE
AS 'pg_logical_emit_message_text';

CREATE OR REPLACE FUNCTION pg_logical_emit_message(
    transactional boolean,
    prefix text,
    message bytea,
    flush boolean DEFAULT false)
RETURNS pg_lsn
LANGUAGE INTERNAL
STRICT VOLATILE
AS 'pg_logical_emit_message_bytea';

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
    IN failover boolean DEFAULT false,
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

CREATE OR REPLACE FUNCTION
  pg_stat_reset_shared(target text DEFAULT NULL)
RETURNS void
LANGUAGE INTERNAL
CALLED ON NULL INPUT VOLATILE PARALLEL SAFE
AS 'pg_stat_reset_shared';

CREATE OR REPLACE FUNCTION
  pg_stat_reset_slru(target text DEFAULT NULL)
RETURNS void
LANGUAGE INTERNAL
CALLED ON NULL INPUT VOLATILE PARALLEL SAFE
AS 'pg_stat_reset_slru';

CREATE OR REPLACE FUNCTION
  pg_set_relation_stats(relation regclass,
                        relpages integer DEFAULT NULL,
                        reltuples real DEFAULT NULL,
                        relallvisible integer DEFAULT NULL)
RETURNS void
LANGUAGE INTERNAL
CALLED ON NULL INPUT VOLATILE
AS 'pg_set_relation_stats';

CREATE OR REPLACE FUNCTION
  pg_set_attribute_stats(relation regclass,
                         attname name,
                         inherited bool,
                         null_frac real DEFAULT NULL,
                         avg_width integer DEFAULT NULL,
                         n_distinct real DEFAULT NULL,
                         most_common_vals text DEFAULT NULL,
                         most_common_freqs real[] DEFAULT NULL,
                         histogram_bounds text DEFAULT NULL,
                         correlation real DEFAULT NULL,
                         most_common_elems text DEFAULT NULL,
                         most_common_elem_freqs real[] DEFAULT NULL,
                         elem_count_histogram real[] DEFAULT NULL,
                         range_length_histogram text DEFAULT NULL,
                         range_empty_frac real DEFAULT NULL,
                         range_bounds_histogram text DEFAULT NULL)
RETURNS void
LANGUAGE INTERNAL
CALLED ON NULL INPUT VOLATILE
AS 'pg_set_attribute_stats';

--
-- The default permissions for functions mean that anyone can execute them.
-- A number of functions shouldn't be executable by just anyone, but rather
-- than use explicit 'superuser()' checks in those functions, we use the GRANT
-- system to REVOKE access to those functions at initdb time.  Administrators
-- can later change who can access these functions, or leave them as only
-- available to superuser / cluster owner, if they choose.
--

REVOKE EXECUTE ON FUNCTION pg_backup_start(text, boolean) FROM public;

REVOKE EXECUTE ON FUNCTION pg_backup_stop(boolean) FROM public;

REVOKE EXECUTE ON FUNCTION pg_create_restore_point(text) FROM public;

REVOKE EXECUTE ON FUNCTION pg_switch_wal() FROM public;

REVOKE EXECUTE ON FUNCTION pg_log_standby_snapshot() FROM public;

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

REVOKE EXECUTE ON FUNCTION pg_stat_have_stats(text, oid, int8) FROM public;

REVOKE EXECUTE ON FUNCTION pg_stat_reset_subscription_stats(oid) FROM public;

REVOKE EXECUTE ON FUNCTION lo_import(text) FROM public;

REVOKE EXECUTE ON FUNCTION lo_import(text, oid) FROM public;

REVOKE EXECUTE ON FUNCTION lo_export(oid, text) FROM public;

REVOKE EXECUTE ON FUNCTION pg_ls_logdir() FROM public;

REVOKE EXECUTE ON FUNCTION pg_ls_waldir() FROM public;

REVOKE EXECUTE ON FUNCTION pg_ls_archive_statusdir() FROM public;

REVOKE EXECUTE ON FUNCTION pg_ls_summariesdir() FROM public;

REVOKE EXECUTE ON FUNCTION pg_ls_tmpdir() FROM public;

REVOKE EXECUTE ON FUNCTION pg_ls_tmpdir(oid) FROM public;

REVOKE EXECUTE ON FUNCTION pg_read_file(text) FROM public;

REVOKE EXECUTE ON FUNCTION pg_read_file(text,boolean) FROM public;

REVOKE EXECUTE ON FUNCTION pg_read_file(text,bigint,bigint) FROM public;

REVOKE EXECUTE ON FUNCTION pg_read_file(text,bigint,bigint,boolean) FROM public;

REVOKE EXECUTE ON FUNCTION pg_read_binary_file(text) FROM public;

REVOKE EXECUTE ON FUNCTION pg_read_binary_file(text,boolean) FROM public;

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

REVOKE EXECUTE ON FUNCTION pg_log_backend_memory_contexts(integer) FROM PUBLIC;

REVOKE EXECUTE ON FUNCTION pg_ls_logicalsnapdir() FROM PUBLIC;

REVOKE EXECUTE ON FUNCTION pg_ls_logicalmapdir() FROM PUBLIC;

REVOKE EXECUTE ON FUNCTION pg_ls_replslotdir(text) FROM PUBLIC;

--
-- We also set up some things as accessible to standard roles.
--

GRANT EXECUTE ON FUNCTION pg_ls_logdir() TO pg_monitor;

GRANT EXECUTE ON FUNCTION pg_ls_waldir() TO pg_monitor;

GRANT EXECUTE ON FUNCTION pg_ls_archive_statusdir() TO pg_monitor;

GRANT EXECUTE ON FUNCTION pg_ls_summariesdir() TO pg_monitor;

GRANT EXECUTE ON FUNCTION pg_ls_tmpdir() TO pg_monitor;

GRANT EXECUTE ON FUNCTION pg_ls_tmpdir(oid) TO pg_monitor;

GRANT EXECUTE ON FUNCTION pg_ls_logicalsnapdir() TO pg_monitor;

GRANT EXECUTE ON FUNCTION pg_ls_logicalmapdir() TO pg_monitor;

GRANT EXECUTE ON FUNCTION pg_ls_replslotdir(text) TO pg_monitor;

GRANT EXECUTE ON FUNCTION pg_current_logfile() TO pg_monitor;

GRANT EXECUTE ON FUNCTION pg_current_logfile(text) TO pg_monitor;

GRANT pg_read_all_settings TO pg_monitor;

GRANT pg_read_all_stats TO pg_monitor;

GRANT pg_stat_scan_tables TO pg_monitor;
