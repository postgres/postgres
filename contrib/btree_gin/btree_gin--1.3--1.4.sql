/* contrib/btree_gin/btree_gin--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gin UPDATE TO '1.4'" to load this file. \quit

--
-- Cross-type operator support is new in 1.4.  We only need to worry
-- about this for cross-type operators that exist in core.
--
-- Because the opclass extractQuery and consistent methods don't directly
-- get any information about the datatype of the RHS value, we have to
-- encode that in the operator strategy numbers.  The strategy numbers
-- are the operator's normal btree strategy (1-5) plus 16 times a code
-- for the RHS datatype.
--

ALTER OPERATOR FAMILY int2_ops USING gin
ADD
    -- Code 1: RHS is int4
    OPERATOR        0x11    < (int2, int4),
    OPERATOR        0x12    <= (int2, int4),
    OPERATOR        0x13    = (int2, int4),
    OPERATOR        0x14    >= (int2, int4),
    OPERATOR        0x15    > (int2, int4),
    -- Code 2: RHS is int8
    OPERATOR        0x21    < (int2, int8),
    OPERATOR        0x22    <= (int2, int8),
    OPERATOR        0x23    = (int2, int8),
    OPERATOR        0x24    >= (int2, int8),
    OPERATOR        0x25    > (int2, int8)
;

ALTER OPERATOR FAMILY int4_ops USING gin
ADD
    -- Code 1: RHS is int2
    OPERATOR        0x11    < (int4, int2),
    OPERATOR        0x12    <= (int4, int2),
    OPERATOR        0x13    = (int4, int2),
    OPERATOR        0x14    >= (int4, int2),
    OPERATOR        0x15    > (int4, int2),
    -- Code 2: RHS is int8
    OPERATOR        0x21    < (int4, int8),
    OPERATOR        0x22    <= (int4, int8),
    OPERATOR        0x23    = (int4, int8),
    OPERATOR        0x24    >= (int4, int8),
    OPERATOR        0x25    > (int4, int8)
;

ALTER OPERATOR FAMILY int8_ops USING gin
ADD
    -- Code 1: RHS is int2
    OPERATOR        0x11    < (int8, int2),
    OPERATOR        0x12    <= (int8, int2),
    OPERATOR        0x13    = (int8, int2),
    OPERATOR        0x14    >= (int8, int2),
    OPERATOR        0x15    > (int8, int2),
    -- Code 2: RHS is int4
    OPERATOR        0x21    < (int8, int4),
    OPERATOR        0x22    <= (int8, int4),
    OPERATOR        0x23    = (int8, int4),
    OPERATOR        0x24    >= (int8, int4),
    OPERATOR        0x25    > (int8, int4)
;
