CREATE TABLE test1 (a int, b text);


CREATE PROCEDURE transaction_test1(x int, y text)
LANGUAGE plpgsql
AS $$
BEGIN
    FOR i IN 0..x LOOP
        INSERT INTO test1 (a, b) VALUES (i, y);
        IF i % 2 = 0 THEN
            COMMIT;
        ELSE
            ROLLBACK;
        END IF;
    END LOOP;
END
$$;

CALL transaction_test1(9, 'foo');

SELECT * FROM test1;


TRUNCATE test1;

DO
LANGUAGE plpgsql
$$
BEGIN
    FOR i IN 0..9 LOOP
        INSERT INTO test1 (a) VALUES (i);
        IF i % 2 = 0 THEN
            COMMIT;
        ELSE
            ROLLBACK;
        END IF;
    END LOOP;
END
$$;

SELECT * FROM test1;


-- transaction commands not allowed when called in transaction block
START TRANSACTION;
CALL transaction_test1(9, 'error');
COMMIT;

START TRANSACTION;
DO LANGUAGE plpgsql $$ BEGIN COMMIT; END $$;
COMMIT;


TRUNCATE test1;

-- not allowed in a function
CREATE FUNCTION transaction_test2() RETURNS int
LANGUAGE plpgsql
AS $$
BEGIN
    FOR i IN 0..9 LOOP
        INSERT INTO test1 (a) VALUES (i);
        IF i % 2 = 0 THEN
            COMMIT;
        ELSE
            ROLLBACK;
        END IF;
    END LOOP;
    RETURN 1;
END
$$;

SELECT transaction_test2();

SELECT * FROM test1;


-- also not allowed if procedure is called from a function
CREATE FUNCTION transaction_test3() RETURNS int
LANGUAGE plpgsql
AS $$
BEGIN
    CALL transaction_test1(9, 'error');
    RETURN 1;
END;
$$;

SELECT transaction_test3();

SELECT * FROM test1;


-- DO block inside function
CREATE FUNCTION transaction_test4() RETURNS int
LANGUAGE plpgsql
AS $$
BEGIN
    EXECUTE 'DO LANGUAGE plpgsql $x$ BEGIN COMMIT; END $x$';
    RETURN 1;
END;
$$;

SELECT transaction_test4();


-- proconfig settings currently disallow transaction statements
CREATE PROCEDURE transaction_test5()
LANGUAGE plpgsql
SET work_mem = 555
AS $$
BEGIN
    COMMIT;
END;
$$;

CALL transaction_test5();


TRUNCATE test1;

-- nested procedure calls
CREATE PROCEDURE transaction_test6(c text)
LANGUAGE plpgsql
AS $$
BEGIN
    CALL transaction_test1(9, c);
END;
$$;

CALL transaction_test6('bar');

SELECT * FROM test1;

TRUNCATE test1;

CREATE PROCEDURE transaction_test7()
LANGUAGE plpgsql
AS $$
BEGIN
    DO 'BEGIN CALL transaction_test1(9, $x$baz$x$); END;';
END;
$$;

CALL transaction_test7();

SELECT * FROM test1;

CREATE PROCEDURE transaction_test8()
LANGUAGE plpgsql
AS $$
BEGIN
    EXECUTE 'CALL transaction_test1(10, $x$baz$x$)';
END;
$$;

CALL transaction_test8();


-- commit inside cursor loop
CREATE TABLE test2 (x int);
INSERT INTO test2 VALUES (0), (1), (2), (3), (4);

TRUNCATE test1;

DO LANGUAGE plpgsql $$
DECLARE
    r RECORD;
BEGIN
    FOR r IN SELECT * FROM test2 ORDER BY x LOOP
        INSERT INTO test1 (a) VALUES (r.x);
        COMMIT;
    END LOOP;
END;
$$;

SELECT * FROM test1;


-- rollback inside cursor loop
TRUNCATE test1;

DO LANGUAGE plpgsql $$
DECLARE
    r RECORD;
BEGIN
    FOR r IN SELECT * FROM test2 ORDER BY x LOOP
        INSERT INTO test1 (a) VALUES (r.x);
        ROLLBACK;
    END LOOP;
END;
$$;

SELECT * FROM test1;


-- commit inside block with exception handler
TRUNCATE test1;

DO LANGUAGE plpgsql $$
BEGIN
    BEGIN
        INSERT INTO test1 (a) VALUES (1);
        COMMIT;
        INSERT INTO test1 (a) VALUES (1/0);
        COMMIT;
    EXCEPTION
        WHEN division_by_zero THEN
            RAISE NOTICE 'caught division_by_zero';
    END;
END;
$$;

SELECT * FROM test1;


-- rollback inside block with exception handler
TRUNCATE test1;

DO LANGUAGE plpgsql $$
BEGIN
    BEGIN
        INSERT INTO test1 (a) VALUES (1);
        ROLLBACK;
        INSERT INTO test1 (a) VALUES (1/0);
        ROLLBACK;
    EXCEPTION
        WHEN division_by_zero THEN
            RAISE NOTICE 'caught division_by_zero';
    END;
END;
$$;

SELECT * FROM test1;


-- COMMIT failures
DO LANGUAGE plpgsql $$
BEGIN
    CREATE TABLE test3 (y int UNIQUE DEFERRABLE INITIALLY DEFERRED);
    COMMIT;
    INSERT INTO test3 (y) VALUES (1);
    COMMIT;
    INSERT INTO test3 (y) VALUES (1);
    INSERT INTO test3 (y) VALUES (2);
    COMMIT;
    INSERT INTO test3 (y) VALUES (3);  -- won't get here
END;
$$;

SELECT * FROM test3;


DROP TABLE test1;
DROP TABLE test2;
DROP TABLE test3;
