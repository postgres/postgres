--
-- Advanced Edge Cases and Real-World Table Tests for DATEDIFF
-- Tests scenarios not covered in basic tests
--

DROP EXTENSION IF EXISTS mssql_compat CASCADE;
CREATE EXTENSION mssql_compat;

-- ============================================================================
-- SECTION 1: EXTREME DATE RANGES
-- ============================================================================

SELECT '=== EXTREME DATE RANGE TESTS ===' AS section;

-- Very old dates (before 1900)
SELECT 'Old dates: 1800s' AS test,
       datediff('year', '1850-06-15', '1900-06-15') AS years_diff,
       datediff('day', '1899-12-31', '1900-01-01') AS day_across_1900;

-- Very future dates
SELECT 'Future dates: 2100s' AS test,
       datediff('year', '2024-01-01', '2100-01-01') AS years_to_2100,
       datediff('month', '2099-06-15', '2100-06-15') AS months_across_century;

-- Huge date spans (1000+ years)
SELECT 'Huge span: 1000 years' AS test,
       datediff('year', '1024-01-01', '2024-01-01') AS millennium,
       datediff('day', '1024-01-01', '2024-01-01') AS days_in_millennium;

-- ============================================================================
-- SECTION 2: BOUNDARY CONDITIONS
-- ============================================================================

SELECT '=== BOUNDARY CONDITION TESTS ===' AS section;

-- First/Last day of year
SELECT 'Year boundaries' AS test,
       datediff('day', '2024-01-01', '2024-12-31') AS full_year_days,
       datediff('year', '2024-01-01', '2024-12-31') AS almost_year;

-- First/Last day of month combinations
SELECT 'Month boundaries: all 12 months' AS test,
       datediff('month', '2024-01-01', '2024-01-31') AS jan,
       datediff('month', '2024-02-01', '2024-02-29') AS feb_leap,
       datediff('month', '2024-03-01', '2024-03-31') AS mar,
       datediff('month', '2024-04-01', '2024-04-30') AS apr;

-- Quarter boundaries
SELECT 'Quarter boundaries' AS test,
       datediff('quarter', '2024-01-01', '2024-03-31') AS q1_full,
       datediff('quarter', '2024-04-01', '2024-06-30') AS q2_full,
       datediff('quarter', '2024-07-01', '2024-09-30') AS q3_full,
       datediff('quarter', '2024-10-01', '2024-12-31') AS q4_full;

-- ============================================================================
-- SECTION 3: LEAP YEAR EDGE CASES
-- ============================================================================

SELECT '=== LEAP YEAR EDGE CASES ===' AS section;

-- Feb 29 to Feb 28 across years
SELECT 'Feb 29 spanning multiple years' AS test,
       datediff('year', '2020-02-29', '2024-02-29') AS leap_to_leap,
       datediff('year', '2024-02-29', '2028-02-29') AS next_leap_cycle;

-- Feb 29 to March 1 in same year
SELECT 'Feb 29 to Mar 1 same year' AS test,
       datediff('day', '2024-02-29', '2024-03-01') AS feb29_to_mar1,
       datediff('month', '2024-02-29', '2024-03-29') AS month_from_feb29;

-- Century years (divisible by 100 but not 400)
SELECT 'Century year edge case (1900 not leap, 2000 is leap)' AS test,
       datediff('day', '1900-02-28', '1900-03-01') AS y1900_not_leap,
       datediff('day', '2000-02-28', '2000-03-01') AS y2000_is_leap;

-- ============================================================================
-- SECTION 4: REAL-WORLD TABLE SCENARIOS
-- ============================================================================

SELECT '=== TABLE-BASED REAL-WORLD TESTS ===' AS section;

-- Create comprehensive test tables
DROP TABLE IF EXISTS employees CASCADE;
DROP TABLE IF EXISTS subscriptions CASCADE;
DROP TABLE IF EXISTS orders CASCADE;
DROP TABLE IF EXISTS contracts CASCADE;

-- Employee table with hire dates
CREATE TABLE employees (
    id SERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    hire_date DATE NOT NULL,
    termination_date DATE,
    department TEXT,
    salary NUMERIC(10,2)
);

INSERT INTO employees (name, hire_date, termination_date, department, salary) VALUES
('Alice Johnson', '2015-03-15', NULL, 'Engineering', 125000),
('Bob Smith', '2018-07-01', NULL, 'Sales', 85000),
('Carol White', '2020-01-10', '2023-06-30', 'Marketing', 72000),
('David Brown', '2019-02-28', NULL, 'Engineering', 115000),
('Eve Davis', '2021-12-31', NULL, 'HR', 68000),
('Frank Miller', '2016-06-15', '2022-12-31', 'Engineering', 130000),
('Grace Lee', '2024-02-29', NULL, 'Sales', 78000),  -- Leap year hire
('Henry Wilson', '2010-01-01', NULL, 'Executive', 250000),
('Ivy Chen', '2023-11-15', NULL, 'Engineering', 95000),
('Jack Taylor', '2017-08-20', NULL, 'Sales', 92000);

-- Subscription table with various billing cycles
CREATE TABLE subscriptions (
    id SERIAL PRIMARY KEY,
    customer_name TEXT NOT NULL,
    plan_type TEXT NOT NULL,
    start_date DATE NOT NULL,
    end_date DATE,
    monthly_rate NUMERIC(8,2),
    billing_cycle TEXT  -- 'monthly', 'quarterly', 'annual'
);

INSERT INTO subscriptions (customer_name, plan_type, start_date, end_date, monthly_rate, billing_cycle) VALUES
('Acme Corp', 'Enterprise', '2023-01-15', '2024-01-15', 999.99, 'annual'),
('Beta Inc', 'Pro', '2023-06-01', '2024-03-18', 199.99, 'monthly'),
('Gamma LLC', 'Basic', '2022-12-31', NULL, 49.99, 'monthly'),
('Delta Co', 'Enterprise', '2024-02-29', NULL, 1499.99, 'annual'),  -- Leap year start
('Echo Systems', 'Pro', '2023-03-15', '2023-09-15', 299.99, 'quarterly'),
('Foxtrot Ltd', 'Basic', '2020-01-01', '2024-01-01', 29.99, 'monthly'),
('Gulf Corp', 'Enterprise', '2021-07-01', NULL, 899.99, 'quarterly'),
('Hotel Inc', 'Pro', '2023-11-30', '2024-02-29', 249.99, 'monthly');  -- End on leap day

-- Orders table for aging analysis
CREATE TABLE orders (
    id SERIAL PRIMARY KEY,
    customer_id INT,
    order_date DATE NOT NULL,
    due_date DATE NOT NULL,
    paid_date DATE,
    amount NUMERIC(12,2)
);

INSERT INTO orders (customer_id, order_date, due_date, paid_date, amount) VALUES
(1, '2024-01-15', '2024-02-14', '2024-02-10', 5000.00),
(2, '2024-02-01', '2024-03-02', NULL, 7500.00),  -- Unpaid
(3, '2023-06-15', '2023-07-15', '2023-09-20', 3200.00),  -- Paid late
(4, '2024-03-01', '2024-03-31', NULL, 12000.00),  -- Unpaid
(1, '2023-12-01', '2024-01-01', '2023-12-28', 8500.00),
(5, '2023-01-01', '2023-02-01', NULL, 4500.00),  -- Very old unpaid
(2, '2024-01-31', '2024-02-29', '2024-02-29', 6000.00),  -- Paid on leap day
(3, '2023-11-15', '2023-12-15', '2024-01-10', 9000.00);

-- Contracts table
CREATE TABLE contracts (
    id SERIAL PRIMARY KEY,
    vendor TEXT NOT NULL,
    start_date DATE NOT NULL,
    end_date DATE NOT NULL,
    value NUMERIC(15,2),
    auto_renew BOOLEAN DEFAULT false
);

INSERT INTO contracts (vendor, start_date, end_date, value, auto_renew) VALUES
('Microsoft', '2022-01-01', '2025-01-01', 500000.00, true),
('AWS', '2023-06-15', '2024-06-15', 250000.00, true),
('Salesforce', '2021-03-01', '2024-02-29', 180000.00, false),  -- Ends on leap day
('Oracle', '2020-07-01', '2023-06-30', 320000.00, false),
('Google', '2024-01-01', '2027-01-01', 450000.00, true);

-- ============================================================================
-- SECTION 5: EMPLOYEE TENURE CALCULATIONS
-- ============================================================================

SELECT '=== EMPLOYEE TENURE ANALYSIS ===' AS section;

-- Current tenure for active employees
SELECT 
    name,
    hire_date,
    datediff('year', hire_date, CURRENT_DATE) AS years_tenure,
    datediff('month', hire_date, CURRENT_DATE) AS months_tenure,
    datediff('day', hire_date, CURRENT_DATE) AS days_tenure,
    CASE 
        WHEN datediff('year', hire_date, CURRENT_DATE) >= 10 THEN 'Veteran (10+ years)'
        WHEN datediff('year', hire_date, CURRENT_DATE) >= 5 THEN 'Senior (5-10 years)'
        WHEN datediff('year', hire_date, CURRENT_DATE) >= 2 THEN 'Mid-level (2-5 years)'
        WHEN datediff('year', hire_date, CURRENT_DATE) >= 1 THEN 'Junior (1-2 years)'
        ELSE 'New hire (<1 year)'
    END AS tenure_level
FROM employees
WHERE termination_date IS NULL
ORDER BY hire_date;

-- Tenure at termination for departed employees
SELECT 
    name,
    hire_date,
    termination_date,
    datediff('year', hire_date, termination_date) AS years_worked,
    datediff('month', hire_date, termination_date) AS months_worked
FROM employees
WHERE termination_date IS NOT NULL
ORDER BY termination_date DESC;

-- Average tenure by department
SELECT 
    department,
    COUNT(*) AS employee_count,
    ROUND(AVG(datediff('year', hire_date, COALESCE(termination_date, CURRENT_DATE))), 2) AS avg_years,
    ROUND(AVG(datediff('month', hire_date, COALESCE(termination_date, CURRENT_DATE))), 2) AS avg_months
FROM employees
GROUP BY department
ORDER BY avg_years DESC;

-- ============================================================================
-- SECTION 6: SUBSCRIPTION BILLING & PRORATION
-- ============================================================================

SELECT '=== SUBSCRIPTION BILLING ANALYSIS ===' AS section;

-- Active subscription duration
SELECT 
    customer_name,
    plan_type,
    start_date,
    COALESCE(end_date, CURRENT_DATE) AS effective_end,
    datediff('month', start_date, COALESCE(end_date, CURRENT_DATE)) AS months_active,
    monthly_rate,
    ROUND((datediff('month', start_date, COALESCE(end_date, CURRENT_DATE)) * monthly_rate)::numeric, 2) AS total_billed
FROM subscriptions
ORDER BY months_active DESC;

-- Proration calculation for partial months
SELECT 
    customer_name,
    start_date,
    end_date,
    monthly_rate,
    datediff('month', start_date, end_date) AS exact_months,
    FLOOR(datediff('month', start_date, end_date)::numeric) AS full_months,
    (datediff('month', start_date, end_date) - FLOOR(datediff('month', start_date, end_date)::numeric)) AS partial_month_fraction,
    ROUND((datediff('month', start_date, end_date) * monthly_rate)::numeric, 2) AS prorated_total
FROM subscriptions
WHERE end_date IS NOT NULL;

-- ============================================================================
-- SECTION 7: ORDER AGING & PAYMENT ANALYSIS
-- ============================================================================

SELECT '=== ORDER AGING ANALYSIS ===' AS section;

-- Aging buckets for unpaid orders
SELECT 
    id AS order_id,
    order_date,
    due_date,
    amount,
    datediff('day', due_date, CURRENT_DATE) AS days_overdue,
    CASE
        WHEN datediff('day', due_date, CURRENT_DATE) <= 0 THEN 'Not Yet Due'
        WHEN datediff('day', due_date, CURRENT_DATE) <= 30 THEN '1-30 Days'
        WHEN datediff('day', due_date, CURRENT_DATE) <= 60 THEN '31-60 Days'
        WHEN datediff('day', due_date, CURRENT_DATE) <= 90 THEN '61-90 Days'
        ELSE '90+ Days'
    END AS aging_bucket
FROM orders
WHERE paid_date IS NULL
ORDER BY days_overdue DESC;

-- Payment timing analysis
SELECT 
    id AS order_id,
    due_date,
    paid_date,
    datediff('day', due_date, paid_date) AS days_from_due,
    CASE
        WHEN datediff('day', due_date, paid_date) < 0 THEN 'Early'
        WHEN datediff('day', due_date, paid_date) = 0 THEN 'On Time'
        WHEN datediff('day', due_date, paid_date) <= 7 THEN 'Within 1 Week'
        WHEN datediff('day', due_date, paid_date) <= 30 THEN 'Within 1 Month'
        ELSE 'Over 1 Month Late'
    END AS payment_status
FROM orders
WHERE paid_date IS NOT NULL;

-- ============================================================================
-- SECTION 8: CONTRACT MANAGEMENT
-- ============================================================================

SELECT '=== CONTRACT ANALYSIS ===' AS section;

-- Contract duration and remaining time
SELECT 
    vendor,
    start_date,
    end_date,
    datediff('year', start_date, end_date) AS contract_years,
    datediff('month', CURRENT_DATE, end_date) AS months_remaining,
    datediff('day', CURRENT_DATE, end_date) AS days_remaining,
    CASE
        WHEN datediff('day', CURRENT_DATE, end_date) < 0 THEN 'Expired'
        WHEN datediff('day', CURRENT_DATE, end_date) <= 30 THEN 'Expiring Soon'
        WHEN datediff('day', CURRENT_DATE, end_date) <= 90 THEN 'Review Soon'
        ELSE 'Active'
    END AS status
FROM contracts
ORDER BY end_date;

-- ============================================================================
-- SECTION 9: WINDOW FUNCTION TESTS
-- ============================================================================

SELECT '=== WINDOW FUNCTION TESTS ===' AS section;

-- Running tenure calculation
SELECT 
    name,
    hire_date,
    department,
    datediff('month', hire_date, CURRENT_DATE) AS months_tenure,
    RANK() OVER (PARTITION BY department ORDER BY datediff('month', hire_date, CURRENT_DATE) DESC) AS dept_tenure_rank,
    SUM(datediff('month', hire_date, CURRENT_DATE)) OVER (PARTITION BY department) AS dept_total_months
FROM employees
WHERE termination_date IS NULL
ORDER BY department, months_tenure DESC;

-- ============================================================================
-- SECTION 10: JOIN AND SUBQUERY TESTS
-- ============================================================================

SELECT '=== JOIN AND SUBQUERY TESTS ===' AS section;

-- Employees with tenure >= average
SELECT 
    e.name,
    e.hire_date,
    datediff('year', e.hire_date, CURRENT_DATE) AS years,
    avg_tenure.avg_years
FROM employees e
CROSS JOIN (
    SELECT ROUND(AVG(datediff('year', hire_date, CURRENT_DATE)), 2) AS avg_years
    FROM employees
    WHERE termination_date IS NULL
) avg_tenure
WHERE termination_date IS NULL
  AND datediff('year', e.hire_date, CURRENT_DATE) >= avg_tenure.avg_years
ORDER BY years DESC;

-- ============================================================================
-- SECTION 11: NULL COLUMN HANDLING
-- ============================================================================

SELECT '=== NULL COLUMN HANDLING ===' AS section;

-- datediff with NULL columns (should return NULL)
SELECT 
    name,
    hire_date,
    termination_date,
    datediff('day', hire_date, termination_date) AS tenure_days,
    COALESCE(datediff('day', hire_date, termination_date)::text, 'Still Active') AS tenure_display
FROM employees
ORDER BY hire_date;

-- ============================================================================
-- SECTION 12: AGGREGATE FUNCTIONS WITH DATEDIFF
-- ============================================================================

SELECT '=== AGGREGATE FUNCTION TESTS ===' AS section;

-- Statistics on subscription durations
SELECT 
    billing_cycle,
    COUNT(*) AS sub_count,
    ROUND(MIN(datediff('month', start_date, COALESCE(end_date, CURRENT_DATE))), 2) AS min_months,
    ROUND(MAX(datediff('month', start_date, COALESCE(end_date, CURRENT_DATE))), 2) AS max_months,
    ROUND(AVG(datediff('month', start_date, COALESCE(end_date, CURRENT_DATE))), 2) AS avg_months,
    ROUND(SUM(monthly_rate * datediff('month', start_date, COALESCE(end_date, CURRENT_DATE)))::numeric, 2) AS total_revenue
FROM subscriptions
GROUP BY billing_cycle
ORDER BY avg_months DESC;

-- ============================================================================
-- SECTION 13: FILTER/WHERE CLAUSE TESTS
-- ============================================================================

SELECT '=== FILTER/WHERE CLAUSE TESTS ===' AS section;

-- Find long-term employees (5+ years)
SELECT name, hire_date, datediff('year', hire_date, CURRENT_DATE) AS years
FROM employees
WHERE termination_date IS NULL
  AND datediff('year', hire_date, CURRENT_DATE) >= 5
ORDER BY years DESC;

-- Find overdue orders over 60 days
SELECT id, due_date, amount, datediff('day', due_date, CURRENT_DATE) AS days_overdue
FROM orders
WHERE paid_date IS NULL
  AND datediff('day', due_date, CURRENT_DATE) > 60
ORDER BY days_overdue DESC;

-- ============================================================================
-- SECTION 14: GROUP BY WITH DATEDIFF EXPRESSIONS
-- ============================================================================

SELECT '=== GROUP BY DATEDIFF TESTS ===' AS section;

-- Group employees by tenure brackets
WITH employee_tenure AS (
    SELECT 
        salary,
        datediff('year', hire_date, CURRENT_DATE) AS years_tenure
    FROM employees
    WHERE termination_date IS NULL
)
SELECT 
    CASE 
        WHEN years_tenure >= 10 THEN '10+ years'
        WHEN years_tenure >= 5 THEN '5-10 years'
        WHEN years_tenure >= 2 THEN '2-5 years'
        ELSE 'Under 2 years'
    END AS tenure_bracket,
    COUNT(*) AS employee_count,
    ROUND(AVG(salary), 2) AS avg_salary
FROM employee_tenure
GROUP BY 1
ORDER BY MIN(years_tenure) DESC;

-- ============================================================================
-- SECTION 15: TIMESTAMPTZ TIMEZONE EDGE CASES
-- ============================================================================

SELECT '=== TIMEZONE EDGE CASES ===' AS section;

-- Same instant, different timezones (should give same result)
SELECT 
    'Same instant different TZ' AS test,
    datediff('day', 
        '2024-06-15 00:00:00+00'::timestamptz, 
        '2024-06-20 00:00:00+00'::timestamptz) AS utc,
    datediff('day', 
        '2024-06-15 00:00:00-08'::timestamptz, 
        '2024-06-20 00:00:00-08'::timestamptz) AS pst;

-- DST transition dates (US)
SELECT 
    'DST transition' AS test,
    datediff('day', '2024-03-09', '2024-03-11') AS across_spring_forward,
    datediff('day', '2024-11-02', '2024-11-04') AS across_fall_back;

-- ============================================================================
-- SECTION 16: PERFORMANCE TEST WITH LARGER DATASET
-- ============================================================================

SELECT '=== PERFORMANCE TEST ===' AS section;

-- Create a larger test table
DROP TABLE IF EXISTS perf_test;
CREATE TABLE perf_test AS
SELECT 
    generate_series AS id,
    '2020-01-01'::date + (random() * 1500)::int AS start_date,
    '2020-01-01'::date + (random() * 1500)::int + (random() * 365)::int AS end_date
FROM generate_series(1, 10000);

-- Time a batch operation
\timing on
SELECT 
    COUNT(*) AS total_rows,
    SUM(datediff('day', start_date, end_date)) AS total_days,
    AVG(datediff('month', start_date, end_date)) AS avg_months
FROM perf_test;
\timing off

-- Cleanup performance test table
DROP TABLE IF EXISTS perf_test;

-- ============================================================================
-- CLEANUP
-- ============================================================================

SELECT '=== CLEANUP ===' AS section;

DROP TABLE IF EXISTS employees CASCADE;
DROP TABLE IF EXISTS subscriptions CASCADE;
DROP TABLE IF EXISTS orders CASCADE;
DROP TABLE IF EXISTS contracts CASCADE;

SELECT 'Advanced edge case tests completed!' AS final_status;

