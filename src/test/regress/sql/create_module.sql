CREATE TABLE cm_test (a int, b text);

CREATE MODULE mtest1
  CREATE FUNCTION m1testa() RETURNS text
     LANGUAGE sql
     RETURN '1x'
  CREATE FUNCTION m1testb() RETURNS text
     LANGUAGE sql
     RETURN '1y';

CREATE SCHEMA temp_mod_test;
GRANT ALL ON SCHEMA temp_mod_test TO public;

CREATE MODULE temp_mod_test.mtest2
  CREATE PROCEDURE m2testa(x text)
  LANGUAGE SQL
  AS $$
  INSERT INTO cm_test VALUES (1, x);
  $$
  CREATE FUNCTION m2testb() RETURNS text
     LANGUAGE sql
     RETURN '2y';

CREATE MODULE mtest3
  CREATE FUNCTION mtest3.m3testa() RETURNS text
     LANGUAGE sql
     RETURN '3x';

SELECT mtest1.m1testa();
SELECT mtest1.m1testb();

SELECT public.mtest1.m1testa();
SELECT public.mtest1.m1testb();

SELECT temp_mod_test.mtest2.m2testb();

SELECT temp_mod_test.mtest2.m2testa('x');  -- error
CALL temp_mod_test.mtest2.m2testa('a');  -- ok
CALL temp_mod_test.mtest2.m2testa('xy' || 'zzy');  -- ok, constant-folded arg

CREATE PROCEDURE mtest1.m1testc(x text)
  LANGUAGE SQL
  AS $$
  INSERT INTO cm_test VALUES (2, x);
  $$;

CALL mtest1.m1testc('a');  -- ok

DROP PROCEDURE mtest1.m1testc(text);
DROP FUNCTION temp_mod_test.mtest2.m2testb();

-- cleanup

DROP MODULE mtest1 CASCADE;
DROP MODULE temp_mod_test.mtest2 CASCADE;

DROP SCHEMA temp_mod_test;
DROP TABLE cm_test;
