--
-- Regression Tests for Label Management
--

--
-- Setup
--
CREATE TABLE t1 (a int, b text);
INSERT INTO t1 VALUES (1, 'aaa'), (2, 'bbb'), (3, 'ccc');
SELECT * INTO t2 FROM t1 WHERE a % 2 = 0;

CREATE FUNCTION f1 () RETURNS text
    AS 'SELECT sepgsql_getcon()'
    LANGUAGE sql;

CREATE FUNCTION f2 () RETURNS text
    AS 'SELECT sepgsql_getcon()'
    LANGUAGE sql;
SECURITY LABEL ON FUNCTION f2()
    IS 'system_u:object_r:sepgsql_trusted_proc_exec_t:s0';

CREATE FUNCTION f3 () RETURNS text
    AS 'BEGIN
          RAISE EXCEPTION ''an exception from f3()'';
          RETURN NULL;
        END;' LANGUAGE plpgsql;
SECURITY LABEL ON FUNCTION f3()
    IS 'system_u:object_r:sepgsql_trusted_proc_exec_t:s0';

CREATE FUNCTION f4 () RETURNS text
    AS 'SELECT sepgsql_getcon()'
    LANGUAGE sql;
SECURITY LABEL ON FUNCTION f4()
    IS 'system_u:object_r:sepgsql_regtest_trusted_proc_exec_t:s0';

--
-- Tests for default labeling behavior
--
-- @SECURITY-CONTEXT=unconfined_u:unconfined_r:sepgsql_regtest_user_t:s0
CREATE TABLE t3 (s int, t text);
INSERT INTO t3 VALUES (1, 'sss'), (2, 'ttt'), (3, 'uuu');

SELECT objtype, objname, label FROM pg_seclabels
    WHERE provider = 'selinux'
     AND  objtype in ('table', 'column')
     AND  objname in ('t1', 't2', 't3');

--
-- Tests for SECURITY LABEL
--
-- @SECURITY-CONTEXT=unconfined_u:unconfined_r:sepgsql_regtest_dba_t:s0
SECURITY LABEL ON TABLE t1
    IS 'system_u:object_r:sepgsql_ro_table_t:s0';	-- ok
SECURITY LABEL ON TABLE t2
    IS 'invalid security context';			-- be failed
SECURITY LABEL ON COLUMN t2
    IS 'system_u:object_r:sepgsql_ro_table_t:s0';	-- be failed
SECURITY LABEL ON COLUMN t2.b
    IS 'system_u:object_r:sepgsql_ro_table_t:s0';	-- ok

--
-- Tests for Trusted Procedures
--
-- @SECURITY-CONTEXT=unconfined_u:unconfined_r:sepgsql_regtest_user_t:s0
SELECT f1();			-- normal procedure
SELECT f2();			-- trusted procedure
SELECT f3();			-- trusted procedure that raises an error
SELECT f4();			-- failed on domain transition
SELECT sepgsql_getcon();	-- client's label must be restored

--
-- Clean up
--
-- @SECURITY-CONTEXT=unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c255
DROP TABLE IF EXISTS t1 CASCADE;
DROP TABLE IF EXISTS t2 CASCADE;
DROP TABLE IF EXISTS t3 CASCADE;
DROP FUNCTION IF EXISTS f1() CASCADE;
DROP FUNCTION IF EXISTS f2() CASCADE;
DROP FUNCTION IF EXISTS f3() CASCADE;
DROP FUNCTION IF EXISTS f4() CASCADE;
