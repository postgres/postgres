/* contrib/pgrowlocks/pgrowlocks--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgrowlocks" to load this file. \quit

CREATE FUNCTION pgrowlocks(IN relname text,
    OUT locked_row TID,		-- row TID
    OUT locker XID,		-- locking XID
    OUT multi bool,		-- multi XID?
    OUT xids xid[],		-- multi XIDs
    OUT modes text[],		-- multi XID statuses
    OUT pids INTEGER[])		-- locker's process id
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pgrowlocks'
LANGUAGE C STRICT;
