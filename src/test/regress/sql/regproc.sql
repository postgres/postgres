--
-- regproc
--

/* If objects exist, return oids */

-- without schemaname

SELECT regoper('||/');
SELECT regproc('now');
SELECT regclass('pg_class');
SELECT regtype('int4');

SELECT to_regoper('||/');
SELECT to_regproc('now');
SELECT to_regclass('pg_class');
SELECT to_regtype('int4');

-- with schemaname

SELECT regoper('pg_catalog.||/');
SELECT regproc('pg_catalog.now');
SELECT regclass('pg_catalog.pg_class');
SELECT regtype('pg_catalog.int4');

SELECT to_regoper('pg_catalog.||/');
SELECT to_regproc('pg_catalog.now');
SELECT to_regclass('pg_catalog.pg_class');
SELECT to_regtype('pg_catalog.int4');

/* If objects don't exist, raise errors. */

-- without schemaname

SELECT regoper('||//');
SELECT regproc('know');
SELECT regclass('pg_classes');
SELECT regtype('int3');

-- with schemaname

SELECT regoper('ng_catalog.||/');
SELECT regproc('ng_catalog.now');
SELECT regclass('ng_catalog.pg_class');
SELECT regtype('ng_catalog.int4');

/* If objects don't exist, return NULL with no error. */

-- without schemaname

SELECT to_regoper('||//');
SELECT to_regproc('know');
SELECT to_regclass('pg_classes');
SELECT to_regtype('int3');

-- with schemaname

SELECT to_regoper('ng_catalog.||/');
SELECT to_regproc('ng_catalog.now');
SELECT to_regclass('ng_catalog.pg_class');
SELECT to_regtype('ng_catalog.int4');
