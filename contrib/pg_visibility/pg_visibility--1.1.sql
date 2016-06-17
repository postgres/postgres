/* contrib/pg_visibility/pg_visibility--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_visibility" to load this file. \quit

-- Show visibility map information.
CREATE FUNCTION pg_visibility_map(regclass, blkno bigint,
								  all_visible OUT boolean,
								  all_frozen OUT boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_visibility_map'
LANGUAGE C STRICT;

-- Show visibility map and page-level visibility information.
CREATE FUNCTION pg_visibility(regclass, blkno bigint,
							  all_visible OUT boolean,
							  all_frozen OUT boolean,
							  pd_all_visible OUT boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_visibility'
LANGUAGE C STRICT;

-- Show visibility map information for each block in a relation.
CREATE FUNCTION pg_visibility_map(regclass, blkno OUT bigint,
								  all_visible OUT boolean,
								  all_frozen OUT boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_visibility_map_rel'
LANGUAGE C STRICT;

-- Show visibility map and page-level visibility information for each block.
CREATE FUNCTION pg_visibility(regclass, blkno OUT bigint,
							  all_visible OUT boolean,
							  all_frozen OUT boolean,
							  pd_all_visible OUT boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_visibility_rel'
LANGUAGE C STRICT;

-- Show summary of visibility map bits for a relation.
CREATE FUNCTION pg_visibility_map_summary(regclass,
    OUT all_visible bigint, OUT all_frozen bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_visibility_map_summary'
LANGUAGE C STRICT;

-- Show tupleids of non-frozen tuples if any in all_frozen pages
-- for a relation.
CREATE FUNCTION pg_check_frozen(regclass, t_ctid OUT tid)
RETURNS SETOF tid
AS 'MODULE_PATHNAME', 'pg_check_frozen'
LANGUAGE C STRICT;

-- Show tupleids of dead tuples if any in all_visible pages for a relation.
CREATE FUNCTION pg_check_visible(regclass, t_ctid OUT tid)
RETURNS SETOF tid
AS 'MODULE_PATHNAME', 'pg_check_visible'
LANGUAGE C STRICT;

-- Truncate the visibility map fork.
CREATE FUNCTION pg_truncate_visibility_map(regclass)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_truncate_visibility_map'
LANGUAGE C STRICT
PARALLEL UNSAFE;  -- let's not make this any more dangerous

-- Don't want these to be available to public.
REVOKE ALL ON FUNCTION pg_visibility_map(regclass, bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_visibility(regclass, bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_visibility_map(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_visibility(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_visibility_map_summary(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_check_frozen(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_check_visible(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_truncate_visibility_map(regclass) FROM PUBLIC;
