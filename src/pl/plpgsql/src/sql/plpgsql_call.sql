--
-- Tests for procedures / CALL syntax
--

CREATE PROCEDURE test_proc1()
LANGUAGE plpgsql
AS $$
BEGIN
    NULL;
END;
$$;

CALL test_proc1();


-- error: can't return non-NULL
CREATE PROCEDURE test_proc2()
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN 5;
END;
$$;


CREATE TABLE test1 (a int);

CREATE PROCEDURE test_proc3(x int)
LANGUAGE plpgsql
AS $$
BEGIN
    INSERT INTO test1 VALUES (x);
END;
$$;

CALL test_proc3(55);

SELECT * FROM test1;


-- Check that plan revalidation doesn't prevent setting transaction properties
-- (bug #18059).  This test must include the first temp-object creation in
-- this script, or it won't exercise the bug scenario.  Hence we put it early.
CREATE PROCEDURE test_proc3a()
LANGUAGE plpgsql
AS $$
BEGIN
   COMMIT;
   SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
   RAISE NOTICE 'done';
END;
$$;

CALL test_proc3a();
CREATE TEMP TABLE tt1(f1 int);
CALL test_proc3a();


-- nested CALL
TRUNCATE TABLE test1;

CREATE PROCEDURE test_proc4(y int)
LANGUAGE plpgsql
AS $$
BEGIN
    CALL test_proc3(y);
    CALL test_proc3($1);
END;
$$;

CALL test_proc4(66);

SELECT * FROM test1;

CALL test_proc4(66);

SELECT * FROM test1;


-- output arguments

CREATE PROCEDURE test_proc5(INOUT a text)
LANGUAGE plpgsql
AS $$
BEGIN
    a := a || '+' || a;
END;
$$;

CALL test_proc5('abc');


CREATE PROCEDURE test_proc6(a int, INOUT b int, INOUT c int)
LANGUAGE plpgsql
AS $$
BEGIN
    b := b * a;
    c := c * a;
END;
$$;

CALL test_proc6(2, 3, 4);


DO
LANGUAGE plpgsql
$$
DECLARE
    x int := 3;
    y int := 4;
BEGIN
    CALL test_proc6(2, x, y);
    RAISE INFO 'x = %, y = %', x, y;
END;
$$;


DO
LANGUAGE plpgsql
$$
DECLARE
    x int := 3;
    y int := 4;
BEGIN
    CALL test_proc6(2, x + 1, y);  -- error
    RAISE INFO 'x = %, y = %', x, y;
END;
$$;


DO
LANGUAGE plpgsql
$$
DECLARE
    x int := 3;
    y int := 4;
BEGIN
    FOR i IN 1..5 LOOP
        CALL test_proc6(i, x, y);
        RAISE INFO 'x = %, y = %', x, y;
    END LOOP;
END;
$$;


-- recursive with output arguments

CREATE PROCEDURE test_proc7(x int, INOUT a int, INOUT b numeric)
LANGUAGE plpgsql
AS $$
BEGIN
IF x > 1 THEN
    a := x / 10;
    b := x / 2;
    CALL test_proc7(b::int, a, b);
END IF;
END;
$$;

CALL test_proc7(100, -1, -1);

-- inner COMMIT with output arguments

CREATE PROCEDURE test_proc7c(x int, INOUT a int, INOUT b numeric)
LANGUAGE plpgsql
AS $$
BEGIN
  a := x / 10;
  b := x / 2;
  COMMIT;
END;
$$;

CREATE PROCEDURE test_proc7cc(_x int)
LANGUAGE plpgsql
AS $$
DECLARE _a int; _b numeric;
BEGIN
  CALL test_proc7c(_x, _a, _b);
  RAISE NOTICE '_x: %,_a: %, _b: %', _x, _a, _b;
END
$$;

CALL test_proc7cc(10);


-- named parameters and defaults

CREATE PROCEDURE test_proc8a(INOUT a int, INOUT b int)
LANGUAGE plpgsql
AS $$
BEGIN
  RAISE NOTICE 'a: %, b: %', a, b;
  a := a * 10;
  b := b + 10;
END;
$$;

CALL test_proc8a(10, 20);
CALL test_proc8a(b => 20, a => 10);

DO $$
DECLARE _a int; _b int;
BEGIN
  _a := 10; _b := 30;
  CALL test_proc8a(_a, _b);
  RAISE NOTICE '_a: %, _b: %', _a, _b;
  CALL test_proc8a(b => _b, a => _a);
  RAISE NOTICE '_a: %, _b: %', _a, _b;
END
$$;


CREATE PROCEDURE test_proc8b(INOUT a int, INOUT b int, INOUT c int)
LANGUAGE plpgsql
AS $$
BEGIN
  RAISE NOTICE 'a: %, b: %, c: %', a, b, c;
  a := a * 10;
  b := b + 10;
  c := c * -10;
END;
$$;

DO $$
DECLARE _a int; _b int; _c int;
BEGIN
  _a := 10; _b := 30; _c := 50;
  CALL test_proc8b(_a, _b, _c);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
  CALL test_proc8b(_a, c => _c, b => _b);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;


CREATE PROCEDURE test_proc8c(INOUT a int, INOUT b int, INOUT c int DEFAULT 11)
LANGUAGE plpgsql
AS $$
BEGIN
  RAISE NOTICE 'a: %, b: %, c: %', a, b, c;
  a := a * 10;
  b := b + 10;
  c := c * -10;
END;
$$;

DO $$
DECLARE _a int; _b int; _c int;
BEGIN
  _a := 10; _b := 30; _c := 50;
  CALL test_proc8c(_a, _b, _c);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
  _a := 10; _b := 30; _c := 50;
  CALL test_proc8c(_a, c => _c, b => _b);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
  _a := 10; _b := 30; _c := 50;
  CALL test_proc8c(c => _c, b => _b, a => _a);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;

DO $$
DECLARE _a int; _b int; _c int;
BEGIN
  _a := 10; _b := 30; _c := 50;
  CALL test_proc8c(_a, _b);  -- fail, no output argument for c
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;

DO $$
DECLARE _a int; _b int; _c int;
BEGIN
  _a := 10; _b := 30; _c := 50;
  CALL test_proc8c(_a, b => _b);  -- fail, no output argument for c
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;

-- polymorphic OUT arguments

CREATE PROCEDURE test_proc12(a anyelement, INOUT b anyelement, INOUT c anyarray)
LANGUAGE plpgsql
AS $$
BEGIN
  RAISE NOTICE 'a: %', a;
  b := a;
  c := array[a];
END;
$$;

DO $$
DECLARE _a int; _b int; _c int[];
BEGIN
  _a := 10;
  CALL test_proc12(_a, _b, _c);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;

DO $$
DECLARE _a int; _b int; _c text[];
BEGIN
  _a := 10;
  CALL test_proc12(_a, _b, _c);  -- error
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;


-- transition variable assignment

TRUNCATE test1;

CREATE FUNCTION triggerfunc1() RETURNS trigger
LANGUAGE plpgsql
AS $$
DECLARE
    z int := 0;
BEGIN
    CALL test_proc6(2, NEW.a, NEW.a);
    RETURN NEW;
END;
$$;

CREATE TRIGGER t1 BEFORE INSERT ON test1 EXECUTE PROCEDURE triggerfunc1();

INSERT INTO test1 VALUES (1), (2), (3);

UPDATE test1 SET a = 22 WHERE a = 2;

SELECT * FROM test1 ORDER BY a;


DROP PROCEDURE test_proc1;
DROP PROCEDURE test_proc3;
DROP PROCEDURE test_proc4;

DROP TABLE test1;


-- more checks for named-parameter handling

CREATE PROCEDURE p1(v_cnt int, v_Text inout text = NULL)
AS $$
BEGIN
  v_Text := 'v_cnt = ' || v_cnt;
END
$$ LANGUAGE plpgsql;

DO $$
DECLARE
  v_Text text;
  v_cnt  integer := 42;
BEGIN
  CALL p1(v_cnt := v_cnt);  -- error, must supply something for v_Text
  RAISE NOTICE '%', v_Text;
END;
$$;

DO $$
DECLARE
  v_Text text;
  v_cnt  integer := 42;
BEGIN
  CALL p1(v_cnt := v_cnt, v_Text := v_Text);
  RAISE NOTICE '%', v_Text;
END;
$$;

DO $$
DECLARE
  v_Text text;
BEGIN
  CALL p1(10, v_Text := v_Text);
  RAISE NOTICE '%', v_Text;
END;
$$;

DO $$
DECLARE
  v_Text text;
  v_cnt  integer;
BEGIN
  CALL p1(v_Text := v_Text, v_cnt := v_cnt);
  RAISE NOTICE '%', v_Text;
END;
$$;


-- check that we detect change of dependencies in CALL
-- atomic and non-atomic call sites do this differently, so check both

CREATE PROCEDURE inner_p (f1 int)
AS $$
BEGIN
  RAISE NOTICE 'inner_p(%)', f1;
END
$$ LANGUAGE plpgsql;

CREATE FUNCTION f(int) RETURNS int AS $$ SELECT $1 + 1 $$ LANGUAGE sql;

CREATE PROCEDURE outer_p (f1 int)
AS $$
BEGIN
  RAISE NOTICE 'outer_p(%)', f1;
  CALL inner_p(f(f1));
END
$$ LANGUAGE plpgsql;

CREATE FUNCTION outer_f (f1 int) RETURNS void
AS $$
BEGIN
  RAISE NOTICE 'outer_f(%)', f1;
  CALL inner_p(f(f1));
END
$$ LANGUAGE plpgsql;

CALL outer_p(42);
SELECT outer_f(42);

DROP FUNCTION f(int);
CREATE FUNCTION f(int) RETURNS int AS $$ SELECT $1 + 2 $$ LANGUAGE sql;

CALL outer_p(42);
SELECT outer_f(42);

-- Check that stable functions in CALL see the correct snapshot

CREATE TABLE t_test (x int);
INSERT INTO t_test VALUES (0);

CREATE FUNCTION f_get_x () RETURNS int
AS $$
DECLARE l_result int;
BEGIN
  SELECT x INTO l_result FROM t_test;
  RETURN l_result;
END
$$ LANGUAGE plpgsql STABLE;

CREATE PROCEDURE f_print_x (x int)
AS $$
BEGIN
  RAISE NOTICE 'f_print_x(%)', x;
END
$$ LANGUAGE plpgsql;

-- test in non-atomic context
DO $$
BEGIN
  UPDATE t_test SET x = x + 1;
  RAISE NOTICE 'f_get_x(%)', f_get_x();
  CALL f_print_x(f_get_x());
  UPDATE t_test SET x = x + 1;
  RAISE NOTICE 'f_get_x(%)', f_get_x();
  CALL f_print_x(f_get_x());
  ROLLBACK;
END
$$;

-- test in non-atomic context, except exception block is locally atomic
DO $$
BEGIN
 BEGIN
  UPDATE t_test SET x = x + 1;
  RAISE NOTICE 'f_get_x(%)', f_get_x();
  CALL f_print_x(f_get_x());
  UPDATE t_test SET x = x + 1;
  RAISE NOTICE 'f_get_x(%)', f_get_x();
  CALL f_print_x(f_get_x());
 EXCEPTION WHEN division_by_zero THEN
   RAISE NOTICE '%', SQLERRM;
 END;
  ROLLBACK;
END
$$;

-- test in atomic context
BEGIN;

DO $$
BEGIN
  UPDATE t_test SET x = x + 1;
  RAISE NOTICE 'f_get_x(%)', f_get_x();
  CALL f_print_x(f_get_x());
  UPDATE t_test SET x = x + 1;
  RAISE NOTICE 'f_get_x(%)', f_get_x();
  CALL f_print_x(f_get_x());
END
$$;

ROLLBACK;
