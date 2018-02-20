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

CALL test_proc2();


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


DROP PROCEDURE test_proc1;
DROP PROCEDURE test_proc2;
DROP PROCEDURE test_proc3;
DROP PROCEDURE test_proc4;

DROP TABLE test1;
