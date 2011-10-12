/* contrib/dblink/dblink--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION dblink" to load this file. \quit

ALTER EXTENSION dblink ADD function dblink_connect(text);
ALTER EXTENSION dblink ADD function dblink_connect(text,text);
ALTER EXTENSION dblink ADD function dblink_connect_u(text);
ALTER EXTENSION dblink ADD function dblink_connect_u(text,text);
ALTER EXTENSION dblink ADD function dblink_disconnect();
ALTER EXTENSION dblink ADD function dblink_disconnect(text);
ALTER EXTENSION dblink ADD function dblink_open(text,text);
ALTER EXTENSION dblink ADD function dblink_open(text,text,boolean);
ALTER EXTENSION dblink ADD function dblink_open(text,text,text);
ALTER EXTENSION dblink ADD function dblink_open(text,text,text,boolean);
ALTER EXTENSION dblink ADD function dblink_fetch(text,integer);
ALTER EXTENSION dblink ADD function dblink_fetch(text,integer,boolean);
ALTER EXTENSION dblink ADD function dblink_fetch(text,text,integer);
ALTER EXTENSION dblink ADD function dblink_fetch(text,text,integer,boolean);
ALTER EXTENSION dblink ADD function dblink_close(text);
ALTER EXTENSION dblink ADD function dblink_close(text,boolean);
ALTER EXTENSION dblink ADD function dblink_close(text,text);
ALTER EXTENSION dblink ADD function dblink_close(text,text,boolean);
ALTER EXTENSION dblink ADD function dblink(text,text);
ALTER EXTENSION dblink ADD function dblink(text,text,boolean);
ALTER EXTENSION dblink ADD function dblink(text);
ALTER EXTENSION dblink ADD function dblink(text,boolean);
ALTER EXTENSION dblink ADD function dblink_exec(text,text);
ALTER EXTENSION dblink ADD function dblink_exec(text,text,boolean);
ALTER EXTENSION dblink ADD function dblink_exec(text);
ALTER EXTENSION dblink ADD function dblink_exec(text,boolean);
ALTER EXTENSION dblink ADD type dblink_pkey_results;
ALTER EXTENSION dblink ADD function dblink_get_pkey(text);
ALTER EXTENSION dblink ADD function dblink_build_sql_insert(text,int2vector,integer,text[],text[]);
ALTER EXTENSION dblink ADD function dblink_build_sql_delete(text,int2vector,integer,text[]);
ALTER EXTENSION dblink ADD function dblink_build_sql_update(text,int2vector,integer,text[],text[]);
ALTER EXTENSION dblink ADD function dblink_current_query();
ALTER EXTENSION dblink ADD function dblink_send_query(text,text);
ALTER EXTENSION dblink ADD function dblink_is_busy(text);
ALTER EXTENSION dblink ADD function dblink_get_result(text);
ALTER EXTENSION dblink ADD function dblink_get_result(text,boolean);
ALTER EXTENSION dblink ADD function dblink_get_connections();
ALTER EXTENSION dblink ADD function dblink_cancel_query(text);
ALTER EXTENSION dblink ADD function dblink_error_message(text);
ALTER EXTENSION dblink ADD function dblink_get_notify();
ALTER EXTENSION dblink ADD function dblink_get_notify(text);
