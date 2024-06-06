CALL nonexistent();  -- error
CALL random();  -- error

CREATE FUNCTION cp_testfunc1(a int) RETURNS int LANGUAGE SQL AS $$ SELECT a $$;

CREATE TABLE cp_test (a int, b text);

CREATE PROCEDURE ptest1(x text)
LANGUAGE SQL
AS $$
INSERT INTO cp_test VALUES (1, x);
$$;

\df ptest1
SELECT pg_get_functiondef('ptest1'::regproc);

-- show only normal functions
\dfn public.*test*1

-- show only procedures
\dfp public.*test*1

SELECT ptest1('x');  -- error
CALL ptest1('a');  -- ok
CALL ptest1('xy' || 'zzy');  -- ok, constant-folded arg
CALL ptest1(substring(random()::numeric(20,15)::text, 1, 1));  -- ok, volatile arg

SELECT * FROM cp_test ORDER BY b COLLATE "C";


CREATE PROCEDURE ptest2()
LANGUAGE SQL
AS $$
SELECT 5;
$$;

CALL ptest2();


-- nested CALL
TRUNCATE cp_test;

CREATE PROCEDURE ptest3(y text)
LANGUAGE SQL
AS $$
CALL ptest1(y);
CALL ptest1($1);
$$;

CALL ptest3('b');

SELECT * FROM cp_test;


-- output arguments

CREATE PROCEDURE ptest4a(INOUT a int, INOUT b int)
LANGUAGE SQL
AS $$
SELECT 1, 2;
$$;

CALL ptest4a(NULL, NULL);

CREATE PROCEDURE ptest4b(INOUT b int, INOUT a int)
LANGUAGE SQL
AS $$
CALL ptest4a(a, b);  -- error, not supported
$$;

-- we used to get confused by a single output argument that is composite
CREATE PROCEDURE ptest4c(INOUT comp int8_tbl)
LANGUAGE SQL
AS $$
SELECT ROW(1, 2)::int8_tbl;
$$;

CALL ptest4c(NULL);

DROP PROCEDURE ptest4a, ptest4c;


-- named and default parameters

CREATE OR REPLACE PROCEDURE ptest5(a int, b text, c int default 100)
LANGUAGE SQL
AS $$
INSERT INTO cp_test VALUES(a, b);
INSERT INTO cp_test VALUES(c, b);
$$;

TRUNCATE cp_test;

CALL ptest5(10, 'Hello', 20);
CALL ptest5(10, 'Hello');
CALL ptest5(10, b => 'Hello');
CALL ptest5(b => 'Hello', a => 10);

SELECT * FROM cp_test;


-- polymorphic types

CREATE PROCEDURE ptest6(a int, b anyelement)
LANGUAGE SQL
AS $$
SELECT NULL::int;
$$;

CALL ptest6(1, 2);

CREATE PROCEDURE ptest6a(inout a anyelement, inout b anyelement)
LANGUAGE SQL
AS $$
SELECT $1, $1;
$$;

CALL ptest6a(1, null);
CALL ptest6a(1.1, null);

CREATE PROCEDURE ptest6b(a anyelement, inout b anyelement, inout c anyarray)
LANGUAGE SQL
AS $$
SELECT $1, array[$1];
$$;

CALL ptest6b(1, null, null);
CALL ptest6b(1.1, null, null);

CREATE PROCEDURE ptest6c(inout a anyelement, inout b anyelement)
LANGUAGE SQL
AS $$
SELECT $1, 1;
$$;

CALL ptest6c(1, null);
CALL ptest6c(1.1, null);  -- fails before v13


-- collation assignment

CREATE PROCEDURE ptest7(a text, b text)
LANGUAGE SQL
AS $$
SELECT a = b;
$$;

CALL ptest7(least('a', 'b'), 'a');


-- various error cases

CALL version();  -- error: not a procedure
CALL sum(1);  -- error: not a procedure

CREATE PROCEDURE ptestx() LANGUAGE SQL WINDOW AS $$ INSERT INTO cp_test VALUES (1, 'a') $$;
CREATE PROCEDURE ptestx() LANGUAGE SQL STRICT AS $$ INSERT INTO cp_test VALUES (1, 'a') $$;
CREATE PROCEDURE ptestx(OUT a int) LANGUAGE SQL AS $$ INSERT INTO cp_test VALUES (1, 'a') $$;

ALTER PROCEDURE ptest1(text) STRICT;
ALTER FUNCTION ptest1(text) VOLATILE;  -- error: not a function
ALTER PROCEDURE cp_testfunc1(int) VOLATILE;  -- error: not a procedure
ALTER PROCEDURE nonexistent() VOLATILE;

DROP FUNCTION ptest1(text);  -- error: not a function
DROP PROCEDURE cp_testfunc1(int);  -- error: not a procedure
DROP PROCEDURE nonexistent();


-- privileges

CREATE USER regress_cp_user1;
GRANT INSERT ON cp_test TO regress_cp_user1;
REVOKE EXECUTE ON PROCEDURE ptest1(text) FROM PUBLIC;
SET ROLE regress_cp_user1;
CALL ptest1('a');  -- error
RESET ROLE;
GRANT EXECUTE ON PROCEDURE ptest1(text) TO regress_cp_user1;
SET ROLE regress_cp_user1;
CALL ptest1('a');  -- ok
RESET ROLE;


-- ROUTINE syntax

ALTER ROUTINE cp_testfunc1(int) RENAME TO cp_testfunc1a;
ALTER ROUTINE cp_testfunc1a RENAME TO cp_testfunc1;

ALTER ROUTINE ptest1(text) RENAME TO ptest1a;
ALTER ROUTINE ptest1a RENAME TO ptest1;

DROP ROUTINE cp_testfunc1(int);


-- cleanup

DROP PROCEDURE ptest1;
DROP PROCEDURE ptest2;

DROP TABLE cp_test;

DROP USER regress_cp_user1;
