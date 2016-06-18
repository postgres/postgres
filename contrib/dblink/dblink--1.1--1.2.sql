/* contrib/dblink/dblink--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION dblink UPDATE TO '1.2'" to load this file. \quit

ALTER FUNCTION dblink_connect(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_connect(text, text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_connect_u(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_connect_u(text, text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_disconnect() PARALLEL RESTRICTED;
ALTER FUNCTION dblink_disconnect(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_open(text, text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_open(text, text, boolean) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_open(text, text, text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_open(text, text, text, boolean) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_fetch(text, int) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_fetch(text, int, boolean) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_fetch(text, text, int) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_fetch(text, text, int, boolean) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_close(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_close(text, boolean) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_close(text, text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_close(text, text, boolean) PARALLEL RESTRICTED;
ALTER FUNCTION dblink(text, text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink(text, text, boolean) PARALLEL RESTRICTED;
ALTER FUNCTION dblink(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink(text, boolean) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_exec(text, text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_exec(text, text, boolean) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_exec(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_exec(text, boolean) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_get_pkey(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_build_sql_insert(text, int2vector, int, _text, _text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_build_sql_delete(text, int2vector, int, _text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_build_sql_update(text, int2vector, int, _text, _text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_current_query() PARALLEL RESTRICTED;
ALTER FUNCTION dblink_send_query(text, text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_is_busy(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_get_result(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_get_result(text, bool) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_get_connections() PARALLEL RESTRICTED;
ALTER FUNCTION dblink_cancel_query(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_error_message(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_get_notify() PARALLEL RESTRICTED;
ALTER FUNCTION dblink_get_notify(text) PARALLEL RESTRICTED;
ALTER FUNCTION dblink_fdw_validator(text[], oid) PARALLEL SAFE;
