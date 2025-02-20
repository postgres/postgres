-- Set datestyle for consistency
SET datestyle TO 'iso, dmy';

-- =====================================================
-- 1. Verify Tables Exist
-- =====================================================
SELECT table_name
FROM information_schema.tables
WHERE table_schema = 'public'
AND table_name IN ('dept', 'emp', 'jobhist');

-- =====================================================
-- 2. Verify Views Exist
-- =====================================================
SELECT table_name
FROM information_schema.views
WHERE table_schema = 'public'
AND table_name = 'salesemp';

-- =====================================================
-- 3. Verify Columns of Tables
-- =====================================================
-- Dept Table
SELECT column_name, data_type
FROM information_schema.columns
WHERE table_name = 'dept'
ORDER BY ordinal_position;

-- Emp Table
SELECT column_name, data_type
FROM information_schema.columns
WHERE table_name = 'emp'
ORDER BY ordinal_position;

-- Jobhist Table
SELECT column_name, data_type
FROM information_schema.columns
WHERE table_name = 'jobhist'
ORDER BY ordinal_position;

-- =====================================================
-- 4. Verify Sequences Exist
-- =====================================================
SELECT relname
FROM pg_class
WHERE relkind = 'S'
AND relname = 'next_empno';

-- =====================================================
-- 5. Verify Functions Exist
-- =====================================================
SELECT proname, prorettype::regtype
FROM pg_proc
JOIN pg_namespace ON pg_proc.pronamespace = pg_namespace.oid
WHERE nspname = 'public'
AND proname IN ('list_emp', 'select_emp', 'emp_query', 'emp_query_caller', 
                'emp_comp', 'new_empno', 'hire_clerk', 'hire_salesman');

-- =====================================================
-- 6. Verify Data in Tables
-- =====================================================
-- Count rows in each table
SELECT 'dept' AS table_name, COUNT(*) FROM dept
UNION ALL
SELECT 'emp', COUNT(*) FROM emp
UNION ALL
SELECT 'jobhist', COUNT(*) FROM jobhist;

-- Check if `emp` employees belong to valid `dept`
SELECT emp.empno, emp.ename, emp.deptno, dept.deptno
FROM emp
LEFT JOIN dept ON emp.deptno = dept.deptno
WHERE dept.deptno IS NULL;

-- Check if `jobhist` records have valid `empno`
SELECT jobhist.empno, jobhist.job, jobhist.sal
FROM jobhist
LEFT JOIN emp ON jobhist.empno = emp.empno
WHERE emp.empno IS NULL;

-- =====================================================
-- 7. Verify Expected Data in Tables
-- =====================================================
-- Sample Data from `dept`
SELECT * FROM dept LIMIT 5;

-- Sample Data from `emp`
SELECT * FROM emp ORDER BY empno LIMIT 5;

-- Sample Data from `jobhist`
SELECT * FROM jobhist ORDER BY empno LIMIT 5;

SELECT * FROM salesemp;

-- Validate if department names follow expected values
SELECT deptno, dname FROM dept
WHERE dname NOT IN ('HR', 'Finance', 'Sales', 'IT', 'Admin');

-- Validate if `emp` salaries are within expected range
SELECT empno, ename, job, sal
FROM emp
WHERE sal < 3000 OR sal > 20000;

-- Check if any employees were hired before 2000 (if expected)
SELECT empno, ename, hiredate FROM emp
WHERE hiredate < '2000-01-01';

-- Verify sequence correctness (Check latest employee number)
-- SELECT last_value FROM next_empno;

-- Verify if function `new_empno()` returns next expected value
-- SELECT new_empno();

-- =====================================================
-- 8. Verify Referential Integrity
-- =====================================================
-- Ensure all employees in `jobhist` exist in `emp`
SELECT jobhist.empno FROM jobhist
LEFT JOIN emp ON jobhist.empno = emp.empno
WHERE emp.empno IS NULL;

-- Ensure `emp.deptno` exists in `dept`
SELECT emp.empno, emp.deptno FROM emp
LEFT JOIN dept ON emp.deptno = dept.deptno
WHERE dept.deptno IS NULL;

-- ===============================================
-- 9. Verify tables are encrypted
-- ===============================================
-- Verify all tables exist and are encrypted
SELECT tablename, pg_tde_is_encrypted(tablename::regclass) AS is_encrypted
FROM pg_tables
WHERE schemaname = 'public'
AND tablename IN ('dept', 'emp', 'jobhist')
ORDER BY tablename;
