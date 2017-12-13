CALL nonexistent();  -- error
CALL random();  -- error

CREATE FUNCTION testfunc1(a int) RETURNS int LANGUAGE SQL AS $$ SELECT a $$;

CREATE TABLE cp_test (a int, b text);

CREATE PROCEDURE ptest1(x text)
LANGUAGE SQL
AS $$
INSERT INTO cp_test VALUES (1, x);
$$;

SELECT ptest1('x');  -- error
CALL ptest1('a');  -- ok

\df ptest1

SELECT * FROM cp_test ORDER BY a;


CREATE PROCEDURE ptest2()
LANGUAGE SQL
AS $$
SELECT 5;
$$;

CALL ptest2();


-- various error cases

CALL version();  -- error: not a procedure
CALL sum(1);  -- error: not a procedure

CREATE PROCEDURE ptestx() LANGUAGE SQL WINDOW AS $$ INSERT INTO cp_test VALUES (1, 'a') $$;
CREATE PROCEDURE ptestx() LANGUAGE SQL STRICT AS $$ INSERT INTO cp_test VALUES (1, 'a') $$;
CREATE PROCEDURE ptestx(OUT a int) LANGUAGE SQL AS $$ INSERT INTO cp_test VALUES (1, 'a') $$;

ALTER PROCEDURE ptest1(text) STRICT;
ALTER FUNCTION ptest1(text) VOLATILE;  -- error: not a function
ALTER PROCEDURE testfunc1(int) VOLATILE;  -- error: not a procedure
ALTER PROCEDURE nonexistent() VOLATILE;

DROP FUNCTION ptest1(text);  -- error: not a function
DROP PROCEDURE testfunc1(int);  -- error: not a procedure
DROP PROCEDURE nonexistent();


-- privileges

CREATE USER regress_user1;
GRANT INSERT ON cp_test TO regress_user1;
REVOKE EXECUTE ON PROCEDURE ptest1(text) FROM PUBLIC;
SET ROLE regress_user1;
CALL ptest1('a');  -- error
RESET ROLE;
GRANT EXECUTE ON PROCEDURE ptest1(text) TO regress_user1;
SET ROLE regress_user1;
CALL ptest1('a');  -- ok
RESET ROLE;


-- ROUTINE syntax

ALTER ROUTINE testfunc1(int) RENAME TO testfunc1a;
ALTER ROUTINE testfunc1a RENAME TO testfunc1;

ALTER ROUTINE ptest1(text) RENAME TO ptest1a;
ALTER ROUTINE ptest1a RENAME TO ptest1;

DROP ROUTINE testfunc1(int);


-- cleanup

DROP PROCEDURE ptest1;
DROP PROCEDURE ptest2;

DROP TABLE cp_test;

DROP USER regress_user1;
