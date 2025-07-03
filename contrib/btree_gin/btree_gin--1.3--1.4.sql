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

ALTER OPERATOR FAMILY float4_ops USING gin
ADD
    -- Code 1: RHS is float8
    OPERATOR        0x11    < (float4, float8),
    OPERATOR        0x12    <= (float4, float8),
    OPERATOR        0x13    = (float4, float8),
    OPERATOR        0x14    >= (float4, float8),
    OPERATOR        0x15    > (float4, float8)
;

ALTER OPERATOR FAMILY float8_ops USING gin
ADD
    -- Code 1: RHS is float4
    OPERATOR        0x11    < (float8, float4),
    OPERATOR        0x12    <= (float8, float4),
    OPERATOR        0x13    = (float8, float4),
    OPERATOR        0x14    >= (float8, float4),
    OPERATOR        0x15    > (float8, float4)
;

ALTER OPERATOR FAMILY text_ops USING gin
ADD
    -- Code 1: RHS is name
    OPERATOR        0x11    < (text, name),
    OPERATOR        0x12    <= (text, name),
    OPERATOR        0x13    = (text, name),
    OPERATOR        0x14    >= (text, name),
    OPERATOR        0x15    > (text, name)
;

ALTER OPERATOR FAMILY name_ops USING gin
ADD
    -- Code 1: RHS is text
    OPERATOR        0x11    < (name, text),
    OPERATOR        0x12    <= (name, text),
    OPERATOR        0x13    = (name, text),
    OPERATOR        0x14    >= (name, text),
    OPERATOR        0x15    > (name, text)
;

ALTER OPERATOR FAMILY date_ops USING gin
ADD
    -- Code 1: RHS is timestamp
    OPERATOR        0x11    < (date, timestamp),
    OPERATOR        0x12    <= (date, timestamp),
    OPERATOR        0x13    = (date, timestamp),
    OPERATOR        0x14    >= (date, timestamp),
    OPERATOR        0x15    > (date, timestamp),
    -- Code 2: RHS is timestamptz
    OPERATOR        0x21    < (date, timestamptz),
    OPERATOR        0x22    <= (date, timestamptz),
    OPERATOR        0x23    = (date, timestamptz),
    OPERATOR        0x24    >= (date, timestamptz),
    OPERATOR        0x25    > (date, timestamptz)
;

ALTER OPERATOR FAMILY timestamp_ops USING gin
ADD
    -- Code 1: RHS is date
    OPERATOR        0x11    < (timestamp, date),
    OPERATOR        0x12    <= (timestamp, date),
    OPERATOR        0x13    = (timestamp, date),
    OPERATOR        0x14    >= (timestamp, date),
    OPERATOR        0x15    > (timestamp, date),
    -- Code 2: RHS is timestamptz
    OPERATOR        0x21    < (timestamp, timestamptz),
    OPERATOR        0x22    <= (timestamp, timestamptz),
    OPERATOR        0x23    = (timestamp, timestamptz),
    OPERATOR        0x24    >= (timestamp, timestamptz),
    OPERATOR        0x25    > (timestamp, timestamptz)
;

ALTER OPERATOR FAMILY timestamptz_ops USING gin
ADD
    -- Code 1: RHS is date
    OPERATOR        0x11    < (timestamptz, date),
    OPERATOR        0x12    <= (timestamptz, date),
    OPERATOR        0x13    = (timestamptz, date),
    OPERATOR        0x14    >= (timestamptz, date),
    OPERATOR        0x15    > (timestamptz, date),
    -- Code 2: RHS is timestamp
    OPERATOR        0x21    < (timestamptz, timestamp),
    OPERATOR        0x22    <= (timestamptz, timestamp),
    OPERATOR        0x23    = (timestamptz, timestamp),
    OPERATOR        0x24    >= (timestamptz, timestamp),
    OPERATOR        0x25    > (timestamptz, timestamp)
;
