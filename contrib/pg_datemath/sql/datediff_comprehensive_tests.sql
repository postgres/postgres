--
-- Comprehensive DATEDIFF Function Tests
-- 50+ tests covering all permutations and edge cases
--

-- Setup: Create extension
DROP EXTENSION IF EXISTS pg_datemath CASCADE;
CREATE EXTENSION pg_datemath;

-- ============================================================================
-- SECTION 1: Test Data Setup
-- ============================================================================

DROP TABLE IF EXISTS date_test_data;
CREATE TABLE date_test_data (
    id SERIAL PRIMARY KEY,
    description TEXT,
    start_date DATE,
    end_date DATE,
    start_ts TIMESTAMP,
    end_ts TIMESTAMP,
    start_tstz TIMESTAMPTZ,
    end_tstz TIMESTAMPTZ
);

-- Insert comprehensive test data
INSERT INTO date_test_data (description, start_date, end_date, start_ts, end_ts, start_tstz, end_tstz) VALUES
-- Basic date ranges
('Same day', '2024-06-15', '2024-06-15', '2024-06-15 10:00:00', '2024-06-15 18:00:00', '2024-06-15 10:00:00+00', '2024-06-15 18:00:00+00'),
('One day apart', '2024-06-15', '2024-06-16', '2024-06-15 00:00:00', '2024-06-16 00:00:00', '2024-06-15 00:00:00+00', '2024-06-16 00:00:00+00'),
('One week apart', '2024-06-01', '2024-06-08', '2024-06-01 12:00:00', '2024-06-08 12:00:00', '2024-06-01 12:00:00+00', '2024-06-08 12:00:00+00'),
('One month apart (same day)', '2024-05-15', '2024-06-15', '2024-05-15 08:30:00', '2024-06-15 08:30:00', '2024-05-15 08:30:00+00', '2024-06-15 08:30:00+00'),
('One quarter apart', '2024-01-01', '2024-04-01', '2024-01-01 00:00:00', '2024-04-01 00:00:00', '2024-01-01 00:00:00+00', '2024-04-01 00:00:00+00'),
('One year apart', '2023-06-15', '2024-06-15', '2023-06-15 15:45:00', '2024-06-15 15:45:00', '2023-06-15 15:45:00+00', '2024-06-15 15:45:00+00'),

-- Leap year scenarios
('Leap year Feb 28 to Mar 1', '2024-02-28', '2024-03-01', '2024-02-28 00:00:00', '2024-03-01 00:00:00', '2024-02-28 00:00:00+00', '2024-03-01 00:00:00+00'),
('Leap year Feb 29 exists', '2024-02-29', '2024-03-01', '2024-02-29 00:00:00', '2024-03-01 00:00:00', '2024-02-29 00:00:00+00', '2024-03-01 00:00:00+00'),
('Non-leap year Feb 28 to Mar 1', '2023-02-28', '2023-03-01', '2023-02-28 00:00:00', '2023-03-01 00:00:00', '2023-02-28 00:00:00+00', '2023-03-01 00:00:00+00'),
('Leap year full year', '2024-01-01', '2025-01-01', '2024-01-01 00:00:00', '2025-01-01 00:00:00', '2024-01-01 00:00:00+00', '2025-01-01 00:00:00+00'),

-- End of month scenarios
('Jan 31 to Feb 28 (non-leap)', '2023-01-31', '2023-02-28', '2023-01-31 00:00:00', '2023-02-28 00:00:00', '2023-01-31 00:00:00+00', '2023-02-28 00:00:00+00'),
('Jan 31 to Feb 29 (leap)', '2024-01-31', '2024-02-29', '2024-01-31 00:00:00', '2024-02-29 00:00:00', '2024-01-31 00:00:00+00', '2024-02-29 00:00:00+00'),
('Mar 31 to Apr 30', '2024-03-31', '2024-04-30', '2024-03-31 00:00:00', '2024-04-30 00:00:00', '2024-03-31 00:00:00+00', '2024-04-30 00:00:00+00'),
('Month end to month end chain', '2024-01-31', '2024-05-31', '2024-01-31 00:00:00', '2024-05-31 00:00:00', '2024-01-31 00:00:00+00', '2024-05-31 00:00:00+00'),

-- Negative spans (start > end)
('Negative: 2 weeks back', '2024-06-22', '2024-06-08', '2024-06-22 00:00:00', '2024-06-08 00:00:00', '2024-06-22 00:00:00+00', '2024-06-08 00:00:00+00'),
('Negative: 3 months back', '2024-09-15', '2024-06-15', '2024-09-15 00:00:00', '2024-06-15 00:00:00', '2024-09-15 00:00:00+00', '2024-06-15 00:00:00+00'),
('Negative: 2 years back', '2026-01-01', '2024-01-01', '2026-01-01 00:00:00', '2024-01-01 00:00:00', '2026-01-01 00:00:00+00', '2024-01-01 00:00:00+00'),

-- Year boundary crossings
('Cross year boundary', '2024-12-31', '2025-01-01', '2024-12-31 23:59:59', '2025-01-01 00:00:01', '2024-12-31 23:59:59+00', '2025-01-01 00:00:01+00'),
('Cross multiple years', '2020-06-15', '2024-06-15', '2020-06-15 00:00:00', '2024-06-15 00:00:00', '2020-06-15 00:00:00+00', '2024-06-15 00:00:00+00'),
('Century boundary', '1999-12-31', '2000-01-01', '1999-12-31 00:00:00', '2000-01-01 00:00:00', '1999-12-31 00:00:00+00', '2000-01-01 00:00:00+00'),

-- Partial periods
('Partial month mid-month', '2024-01-15', '2024-02-20', '2024-01-15 00:00:00', '2024-02-20 00:00:00', '2024-01-15 00:00:00+00', '2024-02-20 00:00:00+00'),
('Partial quarter', '2024-01-15', '2024-05-20', '2024-01-15 00:00:00', '2024-05-20 00:00:00', '2024-01-15 00:00:00+00', '2024-05-20 00:00:00+00'),
('Partial year', '2024-03-15', '2025-06-20', '2024-03-15 00:00:00', '2025-06-20 00:00:00', '2024-03-15 00:00:00+00', '2025-06-20 00:00:00+00'),

-- Employee tenure scenarios
('Employee 90-day probation', '2024-01-15', '2024-04-14', '2024-01-15 09:00:00', '2024-04-14 17:00:00', '2024-01-15 09:00:00+00', '2024-04-14 17:00:00+00'),
('Employee 5-year anniversary', '2019-03-01', '2024-03-01', '2019-03-01 00:00:00', '2024-03-01 00:00:00', '2019-03-01 00:00:00+00', '2024-03-01 00:00:00+00'),
('Employee 10-year tenure', '2014-06-15', '2024-06-15', '2014-06-15 08:00:00', '2024-06-15 08:00:00', '2014-06-15 08:00:00+00', '2024-06-15 08:00:00+00'),

-- Billing/subscription scenarios
('Monthly subscription', '2024-01-01', '2024-01-31', '2024-01-01 00:00:00', '2024-01-31 23:59:59', '2024-01-01 00:00:00+00', '2024-01-31 23:59:59+00'),
('Quarterly billing', '2024-01-01', '2024-03-31', '2024-01-01 00:00:00', '2024-03-31 00:00:00', '2024-01-01 00:00:00+00', '2024-03-31 00:00:00+00'),
('Annual subscription', '2023-07-15', '2024-07-15', '2023-07-15 00:00:00', '2024-07-15 00:00:00', '2023-07-15 00:00:00+00', '2024-07-15 00:00:00+00'),
('Prorated mid-month cancel', '2024-03-01', '2024-03-18', '2024-03-01 00:00:00', '2024-03-18 00:00:00', '2024-03-01 00:00:00+00', '2024-03-18 00:00:00+00'),

-- Large spans
('Decade span', '2010-01-01', '2020-01-01', '2010-01-01 00:00:00', '2020-01-01 00:00:00', '2010-01-01 00:00:00+00', '2020-01-01 00:00:00+00'),
('25 years span', '1999-06-15', '2024-06-15', '1999-06-15 00:00:00', '2024-06-15 00:00:00', '1999-06-15 00:00:00+00', '2024-06-15 00:00:00+00');

-- ============================================================================
-- SECTION 2: DAY Datepart Tests (Tests 1-8)
-- ============================================================================

SELECT '=== DAY DATEPART TESTS ===' AS section;

-- Basic day difference
SELECT 'Basic day difference' AS test_name,
       datediff('day', '2024-01-01', '2024-01-15') AS result,
       14 AS expected,
       CASE WHEN datediff('day', '2024-01-01', '2024-01-15') = 14 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Day difference using 'dd' alias
SELECT 'Day alias dd' AS test_name,
       datediff('dd', '2024-01-01', '2024-01-15') AS result,
       14 AS expected,
       CASE WHEN datediff('dd', '2024-01-01', '2024-01-15') = 14 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Day difference using 'd' alias
SELECT 'Day alias d' AS test_name,
       datediff('d', '2024-03-01', '2024-03-31') AS result,
       30 AS expected,
       CASE WHEN datediff('d', '2024-03-01', '2024-03-31') = 30 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Day difference using 'days' alias
SELECT 'Day alias days' AS test_name,
       datediff('days', '2024-06-01', '2024-06-30') AS result,
       29 AS expected,
       CASE WHEN datediff('days', '2024-06-01', '2024-06-30') = 29 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Negative day difference
SELECT 'Negative day difference' AS test_name,
       datediff('day', '2024-01-15', '2024-01-01') AS result,
       -14 AS expected,
       CASE WHEN datediff('day', '2024-01-15', '2024-01-01') = -14 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Same day returns 0
SELECT 'Same day returns 0' AS test_name,
       datediff('day', '2024-06-15', '2024-06-15') AS result,
       0 AS expected,
       CASE WHEN datediff('day', '2024-06-15', '2024-06-15') = 0 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Leap year February (28th to Mar 1st)
SELECT 'Leap year Feb 28 to Mar 1' AS test_name,
       datediff('day', '2024-02-28', '2024-03-01') AS result,
       2 AS expected,
       CASE WHEN datediff('day', '2024-02-28', '2024-03-01') = 2 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Non-leap year February
SELECT 'Non-leap year Feb 28 to Mar 1' AS test_name,
       datediff('day', '2023-02-28', '2023-03-01') AS result,
       1 AS expected,
       CASE WHEN datediff('day', '2023-02-28', '2023-03-01') = 1 THEN 'PASS' ELSE 'FAIL' END AS status;

-- ============================================================================
-- SECTION 3: WEEK Datepart Tests (Tests 9-15)
-- ============================================================================

SELECT '=== WEEK DATEPART TESTS ===' AS section;

-- Exact 1 week
SELECT 'Exact 1 week' AS test_name,
       datediff('week', '2024-01-01', '2024-01-08') AS result,
       1.000 AS expected,
       CASE WHEN datediff('week', '2024-01-01', '2024-01-08') = 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Exact 2 weeks
SELECT 'Exact 2 weeks' AS test_name,
       datediff('week', '2024-01-01', '2024-01-15') AS result,
       2.000 AS expected,
       CASE WHEN datediff('week', '2024-01-01', '2024-01-15') = 2.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Partial week (9 days = 1.286 weeks)
SELECT 'Partial week 9 days' AS test_name,
       datediff('week', '2024-01-01', '2024-01-10') AS result,
       1.286 AS expected,
       CASE WHEN datediff('week', '2024-01-01', '2024-01-10') = 1.286 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Week alias 'wk'
SELECT 'Week alias wk' AS test_name,
       datediff('wk', '2024-01-01', '2024-01-08') AS result,
       1.000 AS expected,
       CASE WHEN datediff('wk', '2024-01-01', '2024-01-08') = 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Week alias 'ww'
SELECT 'Week alias ww' AS test_name,
       datediff('ww', '2024-01-01', '2024-01-22') AS result,
       3.000 AS expected,
       CASE WHEN datediff('ww', '2024-01-01', '2024-01-22') = 3.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Week alias 'weeks'
SELECT 'Week alias weeks' AS test_name,
       datediff('weeks', '2024-02-01', '2024-02-29') AS result,
       4.000 AS expected,
       CASE WHEN datediff('weeks', '2024-02-01', '2024-02-29') = 4.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Negative weeks
SELECT 'Negative weeks' AS test_name,
       datediff('week', '2024-01-15', '2024-01-01') AS result,
       -2.000 AS expected,
       CASE WHEN datediff('week', '2024-01-15', '2024-01-01') = -2.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- ============================================================================
-- SECTION 4: MONTH Datepart Tests (Tests 16-25)
-- ============================================================================

SELECT '=== MONTH DATEPART TESTS ===' AS section;

-- Aligned month (same day-of-month)
SELECT 'Aligned month same day' AS test_name,
       datediff('month', '2024-01-15', '2024-02-15') AS result,
       1.000 AS expected,
       CASE WHEN datediff('month', '2024-01-15', '2024-02-15') = 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Partial month
SELECT 'Partial month' AS test_name,
       datediff('month', '2024-01-15', '2024-02-20') AS result,
       1.172 AS expected,
       CASE WHEN datediff('month', '2024-01-15', '2024-02-20') = 1.172 THEN 'PASS' ELSE 'FAIL' END AS status;

-- End-of-month alignment (Jan 31 -> Feb 29)
SELECT 'End-of-month alignment' AS test_name,
       datediff('month', '2024-01-31', '2024-02-29') AS result,
       1.000 AS expected,
       CASE WHEN datediff('month', '2024-01-31', '2024-02-29') = 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Month alias 'mm'
SELECT 'Month alias mm' AS test_name,
       datediff('mm', '2024-01-01', '2024-02-01') AS result,
       1.000 AS expected,
       CASE WHEN datediff('mm', '2024-01-01', '2024-02-01') = 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Month alias 'm'
SELECT 'Month alias m' AS test_name,
       datediff('m', '2024-03-15', '2024-06-15') AS result,
       3.000 AS expected,
       CASE WHEN datediff('m', '2024-03-15', '2024-06-15') = 3.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Month alias 'months'
SELECT 'Month alias months' AS test_name,
       datediff('months', '2024-01-01', '2024-07-01') AS result,
       6.000 AS expected,
       CASE WHEN datediff('months', '2024-01-01', '2024-07-01') = 6.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Multiple months with partial
SELECT 'Multiple months partial' AS test_name,
       datediff('month', '2024-01-25', '2024-03-10') AS result,
       1.483 AS expected,
       CASE WHEN datediff('month', '2024-01-25', '2024-03-10') = 1.483 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Negative months
SELECT 'Negative months' AS test_name,
       datediff('month', '2024-06-15', '2024-03-15') AS result,
       -3.000 AS expected,
       CASE WHEN datediff('month', '2024-06-15', '2024-03-15') = -3.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Month spanning year boundary
SELECT 'Month across year boundary' AS test_name,
       datediff('month', '2024-11-15', '2025-02-15') AS result,
       3.000 AS expected,
       CASE WHEN datediff('month', '2024-11-15', '2025-02-15') = 3.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Less than one month
SELECT 'Less than one month' AS test_name,
       datediff('month', '2024-01-01', '2024-01-15') AS result,
       0.452 AS expected,  -- 14 days / 31 days in January
       CASE WHEN datediff('month', '2024-01-01', '2024-01-15') BETWEEN 0.450 AND 0.460 THEN 'PASS' ELSE 'FAIL' END AS status;

-- ============================================================================
-- SECTION 5: QUARTER Datepart Tests (Tests 26-33)
-- ============================================================================

SELECT '=== QUARTER DATEPART TESTS ===' AS section;

-- Exact quarter aligned
SELECT 'Exact quarter aligned' AS test_name,
       datediff('quarter', '2024-01-01', '2024-04-01') AS result,
       1.000 AS expected,
       CASE WHEN datediff('quarter', '2024-01-01', '2024-04-01') = 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Partial quarter
SELECT 'Partial quarter' AS test_name,
       datediff('quarter', '2024-01-15', '2024-05-20') AS result,
       1.385 AS expected,
       CASE WHEN datediff('quarter', '2024-01-15', '2024-05-20') = 1.385 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Quarter alias 'qq'
SELECT 'Quarter alias qq' AS test_name,
       datediff('qq', '2024-01-01', '2024-07-01') AS result,
       2.000 AS expected,
       CASE WHEN datediff('qq', '2024-01-01', '2024-07-01') = 2.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Quarter alias 'q'
SELECT 'Quarter alias q' AS test_name,
       datediff('q', '2024-01-01', '2024-10-01') AS result,
       3.000 AS expected,
       CASE WHEN datediff('q', '2024-01-01', '2024-10-01') = 3.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Quarter alias 'quarters'
SELECT 'Quarter alias quarters' AS test_name,
       datediff('quarters', '2024-01-01', '2025-01-01') AS result,
       4.000 AS expected,
       CASE WHEN datediff('quarters', '2024-01-01', '2025-01-01') = 4.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Negative quarters
SELECT 'Negative quarters' AS test_name,
       datediff('quarter', '2024-10-01', '2024-04-01') AS result,
       -2.000 AS expected,
       CASE WHEN datediff('quarter', '2024-10-01', '2024-04-01') = -2.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Less than one quarter
SELECT 'Less than one quarter' AS test_name,
       datediff('quarter', '2024-01-01', '2024-02-15') AS result,
       0.495 AS expected,  -- ~45 days / 91 days
       CASE WHEN datediff('quarter', '2024-01-01', '2024-02-15') BETWEEN 0.490 AND 0.500 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Quarter across year boundary
SELECT 'Quarter across year boundary' AS test_name,
       datediff('quarter', '2024-10-01', '2025-04-01') AS result,
       2.000 AS expected,
       CASE WHEN datediff('quarter', '2024-10-01', '2025-04-01') = 2.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- ============================================================================
-- SECTION 6: YEAR Datepart Tests (Tests 34-42)
-- ============================================================================

SELECT '=== YEAR DATEPART TESTS ===' AS section;

-- Exact year aligned
SELECT 'Exact year aligned' AS test_name,
       datediff('year', '2024-03-15', '2025-03-15') AS result,
       1.000 AS expected,
       CASE WHEN datediff('year', '2024-03-15', '2025-03-15') = 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Partial year in leap year
SELECT 'Partial year leap year' AS test_name,
       datediff('year', '2024-01-01', '2024-07-01') AS result,
       0.497 AS expected,  -- 182 days / 366
       CASE WHEN datediff('year', '2024-01-01', '2024-07-01') BETWEEN 0.495 AND 0.500 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Year alias 'yy'
SELECT 'Year alias yy' AS test_name,
       datediff('yy', '2020-01-01', '2025-01-01') AS result,
       5.000 AS expected,
       CASE WHEN datediff('yy', '2020-01-01', '2025-01-01') = 5.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Year alias 'yyyy'
SELECT 'Year alias yyyy' AS test_name,
       datediff('yyyy', '2024-06-15', '2027-06-15') AS result,
       3.000 AS expected,
       CASE WHEN datediff('yyyy', '2024-06-15', '2027-06-15') = 3.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Year alias 'y'
SELECT 'Year alias y' AS test_name,
       datediff('y', '2024-01-01', '2024-12-31') AS result,
       0.997 AS expected,  -- 365 days / 366
       CASE WHEN datediff('y', '2024-01-01', '2024-12-31') BETWEEN 0.995 AND 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Year alias 'years'
SELECT 'Year alias years' AS test_name,
       datediff('years', '2014-06-15', '2024-06-15') AS result,
       10.000 AS expected,
       CASE WHEN datediff('years', '2014-06-15', '2024-06-15') = 10.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Year boundary crossing (1 day)
SELECT 'Year boundary 1 day' AS test_name,
       datediff('year', '2024-12-31', '2025-01-01') AS result,
       0.003 AS expected,  -- 1 day / 365
       CASE WHEN datediff('year', '2024-12-31', '2025-01-01') BETWEEN 0.001 AND 0.005 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Negative years
SELECT 'Negative years' AS test_name,
       datediff('year', '2025-06-15', '2020-06-15') AS result,
       -5.000 AS expected,
       CASE WHEN datediff('year', '2025-06-15', '2020-06-15') = -5.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Feb 29 leap year to Feb 28 non-leap (aligned)
SELECT 'Feb 29 to Feb 28 alignment' AS test_name,
       datediff('year', '2024-02-29', '2025-02-28') AS result,
       1.000 AS expected,
       CASE WHEN datediff('year', '2024-02-29', '2025-02-28') = 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- ============================================================================
-- SECTION 7: Case Insensitivity Tests (Tests 43-45)
-- ============================================================================

SELECT '=== CASE INSENSITIVITY TESTS ===' AS section;

-- UPPERCASE datepart
SELECT 'UPPERCASE MONTH' AS test_name,
       datediff('MONTH', '2024-01-01', '2024-02-01') AS result,
       1.000 AS expected,
       CASE WHEN datediff('MONTH', '2024-01-01', '2024-02-01') = 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Mixed case datepart
SELECT 'Mixed case Quarter' AS test_name,
       datediff('QuArTeR', '2024-01-01', '2024-04-01') AS result,
       1.000 AS expected,
       CASE WHEN datediff('QuArTeR', '2024-01-01', '2024-04-01') = 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Mixed case alias
SELECT 'Mixed case alias YY' AS test_name,
       datediff('Yy', '2024-01-01', '2025-01-01') AS result,
       1.000 AS expected,
       CASE WHEN datediff('Yy', '2024-01-01', '2025-01-01') = 1.000 THEN 'PASS' ELSE 'FAIL' END AS status;

-- ============================================================================
-- SECTION 8: TIMESTAMP and TIMESTAMPTZ Tests (Tests 46-48)
-- ============================================================================

SELECT '=== TIMESTAMP TESTS ===' AS section;

-- Timestamp day difference
SELECT 'Timestamp day diff' AS test_name,
       datediff('day', '2024-01-01 10:30:00'::timestamp, '2024-01-15 14:45:00'::timestamp) AS result,
       14 AS expected,
       CASE WHEN datediff('day', '2024-01-01 10:30:00'::timestamp, '2024-01-15 14:45:00'::timestamp) = 14 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Timestamp month difference
SELECT 'Timestamp month diff' AS test_name,
       datediff('month', '2024-01-15 08:00:00'::timestamp, '2024-02-20 16:00:00'::timestamp) AS result,
       1.172 AS expected,
       CASE WHEN datediff('month', '2024-01-15 08:00:00'::timestamp, '2024-02-20 16:00:00'::timestamp) = 1.172 THEN 'PASS' ELSE 'FAIL' END AS status;

-- Timestamptz day difference
SELECT 'Timestamptz day diff' AS test_name,
       datediff('day', '2024-01-01 10:30:00+00'::timestamptz, '2024-01-15 14:45:00+00'::timestamptz) AS result,
       14 AS expected,
       CASE WHEN datediff('day', '2024-01-01 10:30:00+00'::timestamptz, '2024-01-15 14:45:00+00'::timestamptz) = 14 THEN 'PASS' ELSE 'FAIL' END AS status;

-- ============================================================================
-- SECTION 9: Error Handling Tests (Tests 49-50)
-- ============================================================================

SELECT '=== ERROR HANDLING TESTS ===' AS section;

-- Invalid datepart should error
SELECT 'Invalid datepart error' AS test_name;
DO $$
BEGIN
    PERFORM datediff('hour', '2024-01-01'::date, '2024-01-02'::date);
    RAISE NOTICE 'FAIL: No error raised for invalid datepart';
EXCEPTION WHEN invalid_parameter_value THEN
    RAISE NOTICE 'PASS: Correctly raised error for invalid datepart';
END $$;

-- NULL handling (should return NULL)
SELECT 'NULL handling' AS test_name,
       datediff('day', NULL::date, '2024-01-15'::date) IS NULL AS null_start_returns_null,
       datediff('day', '2024-01-01'::date, NULL::date) IS NULL AS null_end_returns_null,
       CASE 
           WHEN datediff('day', NULL::date, '2024-01-15'::date) IS NULL 
            AND datediff('day', '2024-01-01'::date, NULL::date) IS NULL 
           THEN 'PASS' 
           ELSE 'FAIL' 
       END AS status;

-- ============================================================================
-- SECTION 10: Table-Based Tests
-- ============================================================================

SELECT '=== TABLE-BASED TESTS ===' AS section;

-- Test all dateparts against table data
SELECT 
    description,
    datediff('day', start_date, end_date) AS days,
    datediff('week', start_date, end_date) AS weeks,
    datediff('month', start_date, end_date) AS months,
    datediff('quarter', start_date, end_date) AS quarters,
    datediff('year', start_date, end_date) AS years
FROM date_test_data
ORDER BY id;

-- Test with timestamp columns
SELECT 
    description,
    datediff('day', start_ts, end_ts) AS ts_days,
    datediff('month', start_ts, end_ts) AS ts_months
FROM date_test_data
WHERE start_ts IS NOT NULL
ORDER BY id
LIMIT 10;

-- Test with timestamptz columns  
SELECT 
    description,
    datediff('day', start_tstz, end_tstz) AS tstz_days,
    datediff('year', start_tstz, end_tstz) AS tstz_years
FROM date_test_data
WHERE start_tstz IS NOT NULL
ORDER BY id
LIMIT 10;

-- ============================================================================
-- SECTION 11: Aggregation and Analytics Tests
-- ============================================================================

SELECT '=== AGGREGATION TESTS ===' AS section;

-- Average tenure calculations
SELECT 
    'Average differences across test data' AS metric,
    ROUND(AVG(datediff('day', start_date, end_date)), 2) AS avg_days,
    ROUND(AVG(datediff('month', start_date, end_date)), 2) AS avg_months,
    ROUND(AVG(datediff('year', start_date, end_date)), 2) AS avg_years
FROM date_test_data
WHERE start_date <= end_date;

-- Group by ranges
WITH date_diffs AS (
    SELECT 
        id,
        description,
        datediff('day', start_date, end_date) AS day_diff
    FROM date_test_data
    WHERE start_date <= end_date
)
SELECT 
    CASE 
        WHEN day_diff < 7 THEN 'Less than 1 week'
        WHEN day_diff < 30 THEN '1 week to 1 month'
        WHEN day_diff < 90 THEN '1 to 3 months'
        WHEN day_diff < 365 THEN '3 months to 1 year'
        ELSE 'Over 1 year'
    END AS duration_bucket,
    COUNT(*) AS count
FROM date_diffs
GROUP BY 1
ORDER BY MIN(day_diff);

-- ============================================================================
-- SECTION 12: Real-World Scenario Tests
-- ============================================================================

SELECT '=== REAL-WORLD SCENARIO TESTS ===' AS section;

-- Invoice aging report simulation
WITH invoices AS (
    SELECT 
        generate_series(1, 10) AS invoice_id,
        '2024-01-01'::date + (random() * 180)::int AS due_date
)
SELECT 
    invoice_id,
    due_date,
    CURRENT_DATE AS today,
    datediff('day', due_date, CURRENT_DATE) AS days_overdue,
    CASE
        WHEN datediff('day', due_date, CURRENT_DATE) > 90 THEN 'Critical'
        WHEN datediff('day', due_date, CURRENT_DATE) > 60 THEN 'Warning'
        WHEN datediff('day', due_date, CURRENT_DATE) > 30 THEN 'Attention'
        WHEN datediff('day', due_date, CURRENT_DATE) > 0 THEN 'Overdue'
        ELSE 'Current'
    END AS aging_bucket
FROM invoices
ORDER BY days_overdue DESC;

-- Subscription proration simulation
WITH subscriptions AS (
    SELECT 
        1 AS sub_id, '2024-01-15'::date AS start_date, '2024-02-20'::date AS cancel_date, 29.99 AS monthly_rate
    UNION ALL SELECT 
        2, '2024-03-01', '2024-03-18', 49.99
    UNION ALL SELECT 
        3, '2024-06-15', '2024-09-15', 99.99
)
SELECT 
    sub_id,
    start_date,
    cancel_date,
    monthly_rate,
    datediff('month', start_date, cancel_date) AS months_used,
    ROUND((datediff('month', start_date, cancel_date) * monthly_rate)::numeric, 2) AS prorated_charge
FROM subscriptions;

-- Employee tenure report
WITH employees AS (
    SELECT 'Alice' AS name, '2019-03-15'::date AS hire_date
    UNION ALL SELECT 'Bob', '2021-06-01'
    UNION ALL SELECT 'Carol', '2023-01-10'
    UNION ALL SELECT 'David', '2024-06-15'
)
SELECT 
    name,
    hire_date,
    datediff('year', hire_date, CURRENT_DATE) AS years_tenure,
    datediff('month', hire_date, CURRENT_DATE) AS months_tenure,
    datediff('day', hire_date, CURRENT_DATE) AS days_tenure,
    CASE 
        WHEN datediff('year', hire_date, CURRENT_DATE) >= 5 THEN 'Senior'
        WHEN datediff('year', hire_date, CURRENT_DATE) >= 2 THEN 'Mid-level'
        WHEN datediff('year', hire_date, CURRENT_DATE) >= 1 THEN 'Junior'
        ELSE 'Probation'
    END AS tenure_level
FROM employees
ORDER BY hire_date;

-- ============================================================================
-- SUMMARY: Test Results
-- ============================================================================

SELECT '=== TEST SUMMARY ===' AS section;

-- Count passing tests from table data
SELECT 
    'Table data validation' AS category,
    COUNT(*) FILTER (WHERE datediff('day', start_date, end_date) IS NOT NULL) AS day_tests,
    COUNT(*) FILTER (WHERE datediff('week', start_date, end_date) IS NOT NULL) AS week_tests,
    COUNT(*) FILTER (WHERE datediff('month', start_date, end_date) IS NOT NULL) AS month_tests,
    COUNT(*) FILTER (WHERE datediff('quarter', start_date, end_date) IS NOT NULL) AS quarter_tests,
    COUNT(*) FILTER (WHERE datediff('year', start_date, end_date) IS NOT NULL) AS year_tests
FROM date_test_data;

-- Cleanup
DROP TABLE IF EXISTS date_test_data;
-- Note: Keeping extension for further manual testing
-- DROP EXTENSION IF EXISTS pg_datemath;

SELECT 'All comprehensive tests completed!' AS final_status;

