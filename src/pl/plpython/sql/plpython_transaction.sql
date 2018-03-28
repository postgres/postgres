CREATE TABLE test1 (a int, b text);


CREATE PROCEDURE transaction_test1()
LANGUAGE plpythonu
AS $$
for i in range(0, 10):
    plpy.execute("INSERT INTO test1 (a) VALUES (%d)" % i)
    if i % 2 == 0:
        plpy.commit()
    else:
        plpy.rollback()
$$;

CALL transaction_test1();

SELECT * FROM test1;


TRUNCATE test1;

DO
LANGUAGE plpythonu
$$
for i in range(0, 10):
    plpy.execute("INSERT INTO test1 (a) VALUES (%d)" % i)
    if i % 2 == 0:
        plpy.commit()
    else:
        plpy.rollback()
$$;

SELECT * FROM test1;


TRUNCATE test1;

-- not allowed in a function
CREATE FUNCTION transaction_test2() RETURNS int
LANGUAGE plpythonu
AS $$
for i in range(0, 10):
    plpy.execute("INSERT INTO test1 (a) VALUES (%d)" % i)
    if i % 2 == 0:
        plpy.commit()
    else:
        plpy.rollback()
return 1
$$;

SELECT transaction_test2();

SELECT * FROM test1;


-- also not allowed if procedure is called from a function
CREATE FUNCTION transaction_test3() RETURNS int
LANGUAGE plpythonu
AS $$
plpy.execute("CALL transaction_test1()")
return 1
$$;

SELECT transaction_test3();

SELECT * FROM test1;


-- DO block inside function
CREATE FUNCTION transaction_test4() RETURNS int
LANGUAGE plpythonu
AS $$
plpy.execute("DO LANGUAGE plpythonu $x$ plpy.commit() $x$")
return 1
$$;

SELECT transaction_test4();


-- commit inside subtransaction (prohibited)
DO LANGUAGE plpythonu $$
s = plpy.subtransaction()
s.enter()
plpy.commit()
$$;


-- commit inside cursor loop
CREATE TABLE test2 (x int);
INSERT INTO test2 VALUES (0), (1), (2), (3), (4);

TRUNCATE test1;

DO LANGUAGE plpythonu $$
for row in plpy.cursor("SELECT * FROM test2 ORDER BY x"):
    plpy.execute("INSERT INTO test1 (a) VALUES (%s)" % row['x'])
    plpy.commit()
$$;

SELECT * FROM test1;

-- check that this doesn't leak a holdable portal
SELECT * FROM pg_cursors;


-- error in cursor loop with commit
TRUNCATE test1;

DO LANGUAGE plpythonu $$
for row in plpy.cursor("SELECT * FROM test2 ORDER BY x"):
    plpy.execute("INSERT INTO test1 (a) VALUES (12/(%s-2))" % row['x'])
    plpy.commit()
$$;

SELECT * FROM test1;

SELECT * FROM pg_cursors;


-- rollback inside cursor loop
TRUNCATE test1;

DO LANGUAGE plpythonu $$
for row in plpy.cursor("SELECT * FROM test2 ORDER BY x"):
    plpy.execute("INSERT INTO test1 (a) VALUES (%s)" % row['x'])
    plpy.rollback()
$$;

SELECT * FROM test1;

SELECT * FROM pg_cursors;


-- first commit then rollback inside cursor loop
TRUNCATE test1;

DO LANGUAGE plpythonu $$
for row in plpy.cursor("SELECT * FROM test2 ORDER BY x"):
    plpy.execute("INSERT INTO test1 (a) VALUES (%s)" % row['x'])
    if row['x'] % 2 == 0:
        plpy.commit()
    else:
        plpy.rollback()
$$;

SELECT * FROM test1;

SELECT * FROM pg_cursors;


DROP TABLE test1;
DROP TABLE test2;
