--
-- Tests for procedures / CALL syntax
--

CREATE PROCEDURE test_proc1()
LANGUAGE plpythonu
AS $$
pass
$$;

CALL test_proc1();


-- error: can't return non-None
CREATE PROCEDURE test_proc2()
LANGUAGE plpythonu
AS $$
return 5
$$;

CALL test_proc2();


CREATE TABLE test1 (a int);

CREATE PROCEDURE test_proc3(x int)
LANGUAGE plpythonu
AS $$
plpy.execute("INSERT INTO test1 VALUES (%s)" % x)
$$;

CALL test_proc3(55);

SELECT * FROM test1;


-- output arguments

CREATE PROCEDURE test_proc5(INOUT a text)
LANGUAGE plpythonu
AS $$
return [a + '+' + a]
$$;

CALL test_proc5('abc');


CREATE PROCEDURE test_proc6(a int, INOUT b int, INOUT c int)
LANGUAGE plpythonu
AS $$
return (b * a, c * a)
$$;

CALL test_proc6(2, 3, 4);


DROP PROCEDURE test_proc1;
DROP PROCEDURE test_proc2;
DROP PROCEDURE test_proc3;

DROP TABLE test1;
