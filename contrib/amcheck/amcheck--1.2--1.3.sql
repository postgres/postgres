/* contrib/amcheck/amcheck--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION amcheck UPDATE TO '1.3'" to load this file. \quit

--
-- verify_heapam()
--
CREATE FUNCTION verify_heapam(relation regclass,
							  on_error_stop boolean default false,
							  check_toast boolean default false,
							  skip text default 'none',
							  startblock bigint default null,
							  endblock bigint default null,
							  blkno OUT bigint,
							  offnum OUT integer,
							  attnum OUT integer,
							  msg OUT text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'verify_heapam'
LANGUAGE C;

-- Don't want this to be available to public
REVOKE ALL ON FUNCTION verify_heapam(regclass,
									 boolean,
									 boolean,
									 text,
									 bigint,
									 bigint)
FROM PUBLIC;
