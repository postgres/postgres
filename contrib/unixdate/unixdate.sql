-- unixdate
-- Routines to convert int4 (Unix system time) to datetime
--  and int4 (delta time in seconds) to timespan
--
-- Thomas Lockhart (lockhart@alumni.caltech.edu)
-- 1997-11-25
--
-- This cheats and reuses existing code in the standard package.
-- Can not include this directly because built-in functions are optimized
--  into a cache and the duplicate function names abstime_datetime() and
--  reltime_timespan() would result in duplicate constants.
--
-- This works with Postgres v6.2 and higher.

--
-- Conversions from integer to datetime
--

CREATE FUNCTION abstime_datetime(int4)
 RETURNS datetime
 AS '-' LANGUAGE 'internal';

CREATE FUNCTION datetime(int4)
 RETURNS datetime
 AS 'select abstime_datetime($1)' LANGUAGE 'SQL';

CREATE FUNCTION reltime_timespan(int4)
 RETURNS timespan
 AS '-' LANGUAGE 'internal';

CREATE FUNCTION timespan(int4)
 RETURNS timespan
 AS 'select reltime_timespan($1)' LANGUAGE 'SQL';

