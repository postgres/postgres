/* src/test/modules/test_aio/test_aio--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_aio" to load this file. \quit


CREATE FUNCTION errno_from_string(sym text)
RETURNS pg_catalog.int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;


CREATE FUNCTION grow_rel(rel regclass, nblocks int)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;


CREATE FUNCTION modify_rel_block(rel regclass, blockno int,
  zero bool DEFAULT false,
  corrupt_header bool DEFAULT false,
  corrupt_checksum bool DEFAULT false)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION read_rel_block_ll(
    rel regclass,
    blockno int,
    nblocks int DEFAULT 1,
    wait_complete bool DEFAULT true,
    batchmode_enter bool DEFAULT false,
    smgrreleaseall bool DEFAULT false,
    batchmode_exit bool DEFAULT false,
    zero_on_error bool DEFAULT false)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION invalidate_rel_block(rel regclass, blockno int)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION buffer_create_toy(rel regclass, blockno int4)
RETURNS pg_catalog.int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION buffer_call_start_io(buffer int, for_input bool, nowait bool)
RETURNS pg_catalog.bool STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION buffer_call_terminate_io(buffer int, for_input bool, succeed bool, io_error bool, release_aio bool)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;



/*
 * Handle related functions
 */
CREATE FUNCTION handle_get_and_error()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION handle_get_twice()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION handle_get()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION handle_get_release()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION handle_release_last()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;


/*
 * Batchmode related functions
 */
CREATE FUNCTION batch_start()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION batch_end()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;



/*
 * Injection point related functions
 */
CREATE FUNCTION inj_io_short_read_attach(result int)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION inj_io_short_read_detach()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION inj_io_reopen_attach()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION inj_io_reopen_detach()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
