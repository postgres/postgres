--
-- regproc
--

/* If objects exist, return oids */

CREATE ROLE regress_regrole_test;

-- without schemaname

SELECT regoper('||/');
SELECT regoperator('+(int4,int4)');
SELECT regproc('now');
SELECT regprocedure('abs(numeric)');
SELECT regclass('pg_class');
SELECT regtype('int4');

SELECT to_regoper('||/');
SELECT to_regoperator('+(int4,int4)');
SELECT to_regproc('now');
SELECT to_regprocedure('abs(numeric)');
SELECT to_regclass('pg_class');
SELECT to_regtype('int4');

-- with schemaname

SELECT regoper('pg_catalog.||/');
SELECT regoperator('pg_catalog.+(int4,int4)');
SELECT regproc('pg_catalog.now');
SELECT regprocedure('pg_catalog.abs(numeric)');
SELECT regclass('pg_catalog.pg_class');
SELECT regtype('pg_catalog.int4');

SELECT to_regoper('pg_catalog.||/');
SELECT to_regproc('pg_catalog.now');
SELECT to_regprocedure('pg_catalog.abs(numeric)');
SELECT to_regclass('pg_catalog.pg_class');
SELECT to_regtype('pg_catalog.int4');

-- schemaname not applicable

SELECT regrole('regress_regrole_test');
SELECT regrole('"regress_regrole_test"');
SELECT regnamespace('pg_catalog');
SELECT regnamespace('"pg_catalog"');

SELECT to_regrole('regress_regrole_test');
SELECT to_regrole('"regress_regrole_test"');
SELECT to_regnamespace('pg_catalog');
SELECT to_regnamespace('"pg_catalog"');

/* If objects don't exist, raise errors. */

DROP ROLE regress_regrole_test;

-- without schemaname

SELECT regoper('||//');
SELECT regoperator('++(int4,int4)');
SELECT regproc('know');
SELECT regprocedure('absinthe(numeric)');
SELECT regclass('pg_classes');
SELECT regtype('int3');

-- with schemaname

SELECT regoper('ng_catalog.||/');
SELECT regoperator('ng_catalog.+(int4,int4)');
SELECT regproc('ng_catalog.now');
SELECT regprocedure('ng_catalog.abs(numeric)');
SELECT regclass('ng_catalog.pg_class');
SELECT regtype('ng_catalog.int4');

-- schemaname not applicable

SELECT regrole('regress_regrole_test');
SELECT regrole('"regress_regrole_test"');
SELECT regrole('Nonexistent');
SELECT regrole('"Nonexistent"');
SELECT regrole('foo.bar');
SELECT regnamespace('Nonexistent');
SELECT regnamespace('"Nonexistent"');
SELECT regnamespace('foo.bar');

/* If objects don't exist, return NULL with no error. */

-- without schemaname

SELECT to_regoper('||//');
SELECT to_regoperator('++(int4,int4)');
SELECT to_regproc('know');
SELECT to_regprocedure('absinthe(numeric)');
SELECT to_regclass('pg_classes');
SELECT to_regtype('int3');

-- with schemaname

SELECT to_regoper('ng_catalog.||/');
SELECT to_regoperator('ng_catalog.+(int4,int4)');
SELECT to_regproc('ng_catalog.now');
SELECT to_regprocedure('ng_catalog.abs(numeric)');
SELECT to_regclass('ng_catalog.pg_class');
SELECT to_regtype('ng_catalog.int4');

-- schemaname not applicable

SELECT to_regrole('regress_regrole_test');
SELECT to_regrole('"regress_regrole_test"');
SELECT to_regrole('foo.bar');
SELECT to_regrole('Nonexistent');
SELECT to_regrole('"Nonexistent"');
SELECT to_regrole('foo.bar');
SELECT to_regnamespace('Nonexistent');
SELECT to_regnamespace('"Nonexistent"');
SELECT to_regnamespace('foo.bar');
