--
-- Regression Test for DML Permissions
--

--
-- Setup
--
CREATE TABLE t1 (a int, b text);
SECURITY LABEL ON TABLE t1 IS 'system_u:object_r:sepgsql_table_t:s0';
INSERT INTO t1 VALUES (1, 'aaa'), (2, 'bbb'), (3, 'ccc');

CREATE TABLE t2 (x int, y text);
SECURITY LABEL ON TABLE t2 IS 'system_u:object_r:sepgsql_ro_table_t:s0';
INSERT INTO t2 VALUES (1, 'xxx'), (2, 'yyy'), (3, 'zzz');

CREATE TABLE t3 (s int, t text);
SECURITY LABEL ON TABLE t3 IS 'system_u:object_r:sepgsql_fixed_table_t:s0';
INSERT INTO t3 VALUES (1, 'sss'), (2, 'ttt'), (3, 'uuu');

CREATE TABLE t4 (m int, n text);
SECURITY LABEL ON TABLE t4 IS 'system_u:object_r:sepgsql_secret_table_t:s0';
INSERT INTO t4 VALUES (1, 'mmm'), (2, 'nnn'), (3, 'ooo');

CREATE TABLE t5 (e text, f text, g text);
SECURITY LABEL ON TABLE t5 IS 'system_u:object_r:sepgsql_table_t:s0';
SECURITY LABEL ON COLUMN t5.e IS 'system_u:object_r:sepgsql_table_t:s0';
SECURITY LABEL ON COLUMN t5.f IS 'system_u:object_r:sepgsql_ro_table_t:s0';
SECURITY LABEL ON COLUMN t5.g IS 'system_u:object_r:sepgsql_secret_table_t:s0';

---
-- partitioned table parent
CREATE TABLE t1p (o int, p text, q text) PARTITION BY RANGE (o);
SECURITY LABEL ON TABLE t1p IS 'system_u:object_r:sepgsql_table_t:s0';
SECURITY LABEL ON COLUMN t1p.o IS 'system_u:object_r:sepgsql_table_t:s0';
SECURITY LABEL ON COLUMN t1p.p IS 'system_u:object_r:sepgsql_ro_table_t:s0';
SECURITY LABEL ON COLUMN t1p.q IS 'system_u:object_r:sepgsql_secret_table_t:s0';

-- partitioned table children
CREATE TABLE t1p_ones PARTITION OF t1p FOR VALUES FROM ('0') TO ('10');
SECURITY LABEL ON COLUMN t1p_ones.o IS 'system_u:object_r:sepgsql_table_t:s0';
SECURITY LABEL ON COLUMN t1p_ones.p IS 'system_u:object_r:sepgsql_ro_table_t:s0';
SECURITY LABEL ON COLUMN t1p_ones.q IS 'system_u:object_r:sepgsql_secret_table_t:s0';
CREATE TABLE t1p_tens PARTITION OF t1p FOR VALUES FROM ('10') TO ('100');
SECURITY LABEL ON COLUMN t1p_tens.o IS 'system_u:object_r:sepgsql_table_t:s0';
SECURITY LABEL ON COLUMN t1p_tens.p IS 'system_u:object_r:sepgsql_ro_table_t:s0';
SECURITY LABEL ON COLUMN t1p_tens.q IS 'system_u:object_r:sepgsql_secret_table_t:s0';

---

CREATE TABLE customer (cid int primary key, cname text, ccredit text);
SECURITY LABEL ON COLUMN customer.ccredit IS 'system_u:object_r:sepgsql_secret_table_t:s0';
INSERT INTO customer VALUES (1, 'Taro',   '1111-2222-3333-4444'),
                            (2, 'Hanako', '5555-6666-7777-8888');
CREATE FUNCTION customer_credit(int) RETURNS text
    AS 'SELECT regexp_replace(ccredit, ''-[0-9]+$'', ''-????'') FROM customer WHERE cid = $1'
    LANGUAGE sql;
SECURITY LABEL ON FUNCTION customer_credit(int)
    IS 'system_u:object_r:sepgsql_trusted_proc_exec_t:s0';

SELECT objtype, objname, label FROM pg_seclabels
    WHERE provider = 'selinux'
     AND  objtype in ('table', 'column')
     AND  objname in ('t1', 't2', 't3', 't4',
                      't5', 't5.e', 't5.f', 't5.g',
                      't1p', 't1p.o', 't1p.p', 't1p.q',
                      't1p_ones', 't1p_ones.o', 't1p_ones.p', 't1p_ones.q',
                      't1p_tens', 't1p_tens.o', 't1p_tens.p', 't1p_tens.q')
ORDER BY objname COLLATE "C";

CREATE SCHEMA my_schema_1;
CREATE TABLE my_schema_1.ts1 (a int, b text);
CREATE TABLE my_schema_1.pts1 (o int, p text) PARTITION BY RANGE (o);
CREATE TABLE my_schema_1.pts1_ones PARTITION OF my_schema_1.pts1 FOR VALUES FROM ('0') to ('10');

CREATE SCHEMA my_schema_2;
CREATE TABLE my_schema_2.ts2 (x int, y text);
CREATE TABLE my_schema_2.pts2 (o int, p text) PARTITION BY RANGE (o);
CREATE TABLE my_schema_2.pts2_tens PARTITION OF my_schema_2.pts2 FOR VALUES FROM ('10') to ('100');

SECURITY LABEL ON SCHEMA my_schema_2
    IS 'system_u:object_r:sepgsql_regtest_invisible_schema_t:s0';

-- Hardwired Rules
UPDATE pg_attribute SET attisdropped = true
    WHERE attrelid = 't5'::regclass AND attname = 'f';	-- failed

--
-- Simple DML statements
--
-- @SECURITY-CONTEXT=unconfined_u:unconfined_r:sepgsql_regtest_user_t:s0

SELECT * FROM t1;			-- ok
SELECT * FROM t2;			-- ok
SELECT * FROM t3;			-- ok
SELECT * FROM t4;			-- failed
SELECT * FROM t5;			-- failed
SELECT e,f FROM t5;			-- ok

---
-- partitioned table parent
SELECT * FROM t1p;			-- failed
SELECT o,p FROM t1p;		-- ok
--partitioned table children
SELECT * FROM t1p_ones;			-- failed
SELECT o FROM t1p_ones;			-- ok
SELECT o,p FROM t1p_ones;		-- ok
SELECT * FROM t1p_tens;			-- failed
SELECT o FROM t1p_tens;			-- ok
SELECT o,p FROM t1p_tens;		-- ok
---

SELECT * FROM customer;									-- failed
SELECT cid, cname, customer_credit(cid) FROM customer;	-- ok

SELECT count(*) FROM t5;					-- ok
SELECT count(*) FROM t5 WHERE g IS NULL;	-- failed

---
-- partitioned table parent
SELECT count(*) FROM t1p;					-- ok
SELECT count(*) FROM t1p WHERE q IS NULL;	-- failed
-- partitioned table children
SELECT count(*) FROM t1p_ones;					-- ok
SELECT count(*) FROM t1p_ones WHERE q IS NULL;	-- failed
SELECT count(*) FROM t1p_tens;					-- ok
SELECT count(*) FROM t1p_tens WHERE q IS NULL;	-- failed
---

INSERT INTO t1 VALUES (4, 'abc');		-- ok
INSERT INTO t2 VALUES (4, 'xyz');		-- failed
INSERT INTO t3 VALUES (4, 'stu');		-- ok
INSERT INTO t4 VALUES (4, 'mno');		-- failed
INSERT INTO t5 VALUES (1,2,3);			-- failed
INSERT INTO t5 (e,f) VALUES ('abc', 'def');	-- failed
INSERT INTO t5 (e) VALUES ('abc');		-- ok

---
-- partitioned table parent
INSERT INTO t1p (o,p) VALUES (9, 'mno');		-- failed
INSERT INTO t1p (o) VALUES (9);						-- ok
INSERT INTO t1p (o,p) VALUES (99, 'pqr');		-- failed
INSERT INTO t1p (o) VALUES (99);					-- ok
-- partitioned table children
INSERT INTO t1p_ones (o,p) VALUES (9, 'mno');		-- failed
INSERT INTO t1p_ones (o) VALUES (9);				-- ok
INSERT INTO t1p_tens (o,p) VALUES (99, 'pqr');		-- failed
INSERT INTO t1p_tens (o) VALUES (99);				-- ok
---

UPDATE t1 SET b = b || '_upd';			-- ok
UPDATE t2 SET y = y || '_upd';			-- failed
UPDATE t3 SET t = t || '_upd';			-- failed
UPDATE t4 SET n = n || '_upd';			-- failed
UPDATE t5 SET e = 'xyz';			-- ok
UPDATE t5 SET e = f || '_upd';			-- ok
UPDATE t5 SET e = g || '_upd';			-- failed

---
-- partitioned table parent
UPDATE t1p SET o = 9 WHERE o < 10;			-- ok
UPDATE t1p SET o = 99 WHERE o >= 10;			-- ok
UPDATE t1p SET o = ascii(COALESCE(p,'upd'))%10 WHERE o < 10;		-- ok
UPDATE t1p SET o = ascii(COALESCE(q,'upd'))%100 WHERE o >= 10;	-- failed
-- partitioned table children
UPDATE t1p_ones SET o = 9;								-- ok
UPDATE t1p_ones SET o = ascii(COALESCE(p,'upd'))%10;	-- ok
UPDATE t1p_ones SET o = ascii(COALESCE(q,'upd'))%10;	-- failed
UPDATE t1p_tens SET o = 99;								-- ok
UPDATE t1p_tens SET o = ascii(COALESCE(p,'upd'))%100;	-- ok
UPDATE t1p_tens SET o = ascii(COALESCE(q,'upd'))%100;	-- failed
---

DELETE FROM t1;					-- ok
DELETE FROM t2;					-- failed
DELETE FROM t3;					-- failed
DELETE FROM t4;					-- failed
DELETE FROM t5;					-- ok
DELETE FROM t5 WHERE f IS NULL;			-- ok
DELETE FROM t5 WHERE g IS NULL;			-- failed

---
-- partitioned table parent
DELETE FROM t1p;						-- ok
DELETE FROM t1p WHERE p IS NULL;		-- ok
DELETE FROM t1p WHERE q IS NULL;		-- failed
-- partitioned table children
DELETE FROM t1p_ones WHERE p IS NULL;		-- ok
DELETE FROM t1p_ones WHERE q IS NULL;		-- failed;
DELETE FROM t1p_tens WHERE p IS NULL;		-- ok
DELETE FROM t1p_tens WHERE q IS NULL;		-- failed
---

--
-- COPY TO/FROM statements
--
COPY t1 TO '/dev/null';				-- ok
COPY t2 TO '/dev/null';				-- ok
COPY t3 TO '/dev/null';				-- ok
COPY t4 TO '/dev/null';				-- failed
COPY t5 TO '/dev/null';				-- failed
COPY t5(e,f) TO '/dev/null';			-- ok

---
-- partitioned table parent
COPY (SELECT * FROM t1p) TO '/dev/null';		-- failed
COPY (SELECT (o,p) FROM t1p) TO '/dev/null';	-- ok
-- partitioned table children
COPY t1p_ones TO '/dev/null';				-- failed
COPY t1p_ones(o,p) TO '/dev/null';			-- ok
COPY t1p_tens TO '/dev/null';				-- failed
COPY t1p_tens(o,p) TO '/dev/null';			-- ok
---

COPY t1 FROM '/dev/null';			-- ok
COPY t2 FROM '/dev/null';			-- failed
COPY t3 FROM '/dev/null';			-- ok
COPY t4 FROM '/dev/null';			-- failed
COPY t5 FROM '/dev/null';			-- failed
COPY t5 (e,f) FROM '/dev/null';			-- failed
COPY t5 (e) FROM '/dev/null';			-- ok

---
-- partitioned table parent
COPY t1p FROM '/dev/null';				-- failed
COPY t1p (o) FROM '/dev/null';			-- ok
-- partitioned table children
COPY t1p_ones FROM '/dev/null';				-- failed
COPY t1p_ones (o) FROM '/dev/null';			-- ok
COPY t1p_tens FROM '/dev/null';				-- failed
COPY t1p_tens (o) FROM '/dev/null';			-- ok
---

--
-- Schema search path
--
SET search_path = my_schema_1, my_schema_2, public;
SELECT * FROM ts1;		-- ok
SELECT * FROM ts2;		-- failed (relation not found)
SELECT * FROM my_schema_2.ts2;	-- failed (policy violation)

--
-- Clean up
--
-- @SECURITY-CONTEXT=unconfined_u:unconfined_r:sepgsql_regtest_superuser_t:s0-s0:c0.c255
DROP TABLE IF EXISTS t1 CASCADE;
DROP TABLE IF EXISTS t2 CASCADE;
DROP TABLE IF EXISTS t3 CASCADE;
DROP TABLE IF EXISTS t4 CASCADE;
DROP TABLE IF EXISTS t5 CASCADE;
DROP TABLE IF EXISTS t1p CASCADE;
DROP TABLE IF EXISTS customer CASCADE;
DROP SCHEMA IF EXISTS my_schema_1 CASCADE;
DROP SCHEMA IF EXISTS my_schema_2 CASCADE;
