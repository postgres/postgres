/* contrib/mssql_compat/mssql_compat--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION mssql_compat" to load this file. \quit

--
-- datediff(datepart, start_date, end_date) - SQL Server compatible date difference
--
-- Returns the difference between two dates in the specified datepart unit.
-- Supports: year, quarter, month, week, day (and aliases)
--
-- Unlike SQL Server's boundary-crossing semantics, this implementation provides
-- mathematically accurate results using a hybrid calculation model: full calendar
-- units plus contextual fractions based on actual period lengths.
--

-- Date version
CREATE FUNCTION datediff(
    datepart TEXT,
    start_date DATE,
    end_date DATE
)
RETURNS NUMERIC
AS 'MODULE_PATHNAME', 'datediff_date'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION datediff(TEXT, DATE, DATE) IS
'Calculate the difference between two dates in the specified unit (year, quarter, month, week, day)';

-- Timestamp version
CREATE FUNCTION datediff(
    datepart TEXT,
    start_ts TIMESTAMP,
    end_ts TIMESTAMP
)
RETURNS NUMERIC
AS 'MODULE_PATHNAME', 'datediff_timestamp'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION datediff(TEXT, TIMESTAMP, TIMESTAMP) IS
'Calculate the difference between two timestamps in the specified unit (year, quarter, month, week, day)';

-- Timestamptz version
CREATE FUNCTION datediff(
    datepart TEXT,
    start_tstz TIMESTAMPTZ,
    end_tstz TIMESTAMPTZ
)
RETURNS NUMERIC
AS 'MODULE_PATHNAME', 'datediff_timestamptz'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION datediff(TEXT, TIMESTAMPTZ, TIMESTAMPTZ) IS
'Calculate the difference between two timestamps with timezone in the specified unit (year, quarter, month, week, day)';

