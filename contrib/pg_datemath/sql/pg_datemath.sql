--
-- Test cases for pg_datemath extension
-- Tests datediff function with various dateparts and edge cases
--

CREATE EXTENSION pg_datemath;

--
-- Basic Day Calculations
--
SELECT 'Day difference basic' AS test;
SELECT datediff('day', '2024-01-01'::date, '2024-01-15'::date);

SELECT 'Day difference negative' AS test;
SELECT datediff('day', '2024-01-15'::date, '2024-01-01'::date);

--
-- Week Calculations
--
SELECT 'Week exact' AS test;
SELECT datediff('week', '2024-01-01'::date, '2024-01-08'::date);

SELECT 'Week partial' AS test;
SELECT datediff('week', '2024-01-01'::date, '2024-01-10'::date);

--
-- Month Calculations
--
SELECT 'Month aligned' AS test;
SELECT datediff('month', '2024-01-15'::date, '2024-02-15'::date);

SELECT 'Month partial' AS test;
SELECT datediff('month', '2024-01-15'::date, '2024-02-20'::date);

SELECT 'Month end-of-month alignment' AS test;
SELECT datediff('month', '2024-01-31'::date, '2024-02-29'::date);

--
-- Quarter Calculations
--
SELECT 'Quarter aligned' AS test;
SELECT datediff('quarter', '2024-01-01'::date, '2024-04-01'::date);

SELECT 'Quarter partial' AS test;
SELECT datediff('quarter', '2024-01-15'::date, '2024-05-20'::date);

--
-- Year Calculations
--
SELECT 'Year aligned' AS test;
SELECT datediff('year', '2024-03-15'::date, '2025-03-15'::date);

SELECT 'Year partial leap year' AS test;
SELECT datediff('year', '2024-01-01'::date, '2024-07-01'::date);

--
-- NULL Handling - STRICT functions return NULL for NULL inputs
--
SELECT 'NULL start date' AS test;
SELECT datediff('day', NULL::date, '2024-01-15'::date);

SELECT 'NULL end date' AS test;
SELECT datediff('day', '2024-01-01'::date, NULL::date);

--
-- Invalid Datepart
--
SELECT 'Invalid datepart' AS test;
SELECT datediff('hour', '2024-01-01'::date, '2024-01-02'::date);

--
-- Case Insensitivity
--
SELECT 'Case insensitive datepart' AS test;
SELECT datediff('MONTH', '2024-01-01'::date, '2024-02-01'::date);
SELECT datediff('Month', '2024-01-01'::date, '2024-02-01'::date);
SELECT datediff('month', '2024-01-01'::date, '2024-02-01'::date);

--
-- Edge Cases
--
SELECT 'Same date' AS test;
SELECT datediff('day', '2024-01-01'::date, '2024-01-01'::date);

SELECT 'Leap year February 29' AS test;
SELECT datediff('day', '2024-02-28'::date, '2024-03-01'::date);

SELECT 'Non-leap year February' AS test;
SELECT datediff('day', '2023-02-28'::date, '2023-03-01'::date);

SELECT 'Year boundary' AS test;
SELECT datediff('year', '2024-12-31'::date, '2025-01-01'::date);

SELECT 'Multi-year span' AS test;
SELECT datediff('year', '2020-01-01'::date, '2025-01-01'::date);

SELECT 'Century boundary' AS test;
SELECT datediff('day', '1999-12-31'::date, '2000-01-01'::date);

--
-- Alias Tests
--
SELECT 'Alias: yy for year' AS test;
SELECT datediff('yy', '2024-01-01'::date, '2025-01-01'::date);

SELECT 'Alias: yyyy for year' AS test;
SELECT datediff('yyyy', '2024-01-01'::date, '2025-01-01'::date);

SELECT 'Alias: mm for month' AS test;
SELECT datediff('mm', '2024-01-15'::date, '2024-02-15'::date);

SELECT 'Alias: qq for quarter' AS test;
SELECT datediff('qq', '2024-01-01'::date, '2024-04-01'::date);

SELECT 'Alias: wk for week' AS test;
SELECT datediff('wk', '2024-01-01'::date, '2024-01-08'::date);

SELECT 'Alias: dd for day' AS test;
SELECT datediff('dd', '2024-01-01'::date, '2024-01-15'::date);

--
-- Timestamp Tests
--
SELECT 'Timestamp: basic day diff' AS test;
SELECT datediff('day', '2024-01-01 10:30:00'::timestamp, '2024-01-15 14:45:00'::timestamp);

SELECT 'Timestamp: month diff' AS test;
SELECT datediff('month', '2024-01-15 08:00:00'::timestamp, '2024-02-20 16:00:00'::timestamp);

--
-- Timestamptz Tests
--
SELECT 'Timestamptz: basic day diff' AS test;
SELECT datediff('day', '2024-01-01 10:30:00+00'::timestamptz, '2024-01-15 14:45:00+00'::timestamptz);

--
-- Additional Month Calculation Tests
--
SELECT 'Month: Jan 25 to Mar 10' AS test;
SELECT datediff('month', '2024-01-25'::date, '2024-03-10'::date);

SELECT 'Month: subscription proration example' AS test;
SELECT datediff('month', '2024-01-15'::date, '2024-02-20'::date);

--
-- Additional Quarter Calculation Tests
--
SELECT 'Quarter: Q1 to Q2 partial' AS test;
SELECT datediff('quarter', '2024-01-15'::date, '2024-05-20'::date);

--
-- Additional Year Calculation Tests
--
SELECT 'Year: partial year calculation' AS test;
SELECT datediff('year', '2024-03-15'::date, '2025-06-20'::date);

SELECT 'Year: exact 5-year tenure' AS test;
SELECT datediff('year', '2020-03-15'::date, '2025-03-15'::date);

SELECT 'Year: leap year partial (182 days / 366)' AS test;
SELECT datediff('year', '2024-01-01'::date, '2024-07-01'::date);

--
-- Week Calculation Additional Tests
--
SELECT 'Week: exact 2 weeks' AS test;
SELECT datediff('week', '2024-01-01'::date, '2024-01-15'::date);

SELECT 'Week: 9 days example' AS test;
SELECT datediff('week', '2024-01-01'::date, '2024-01-10'::date);

DROP EXTENSION pg_datemath;

