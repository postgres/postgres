-- This makes sure the pg_attribute columns match the type's columns
-- bjm 1998/08/26

-- check lengths
SELECT	pg_attribute.oid, relname, attname
FROM	pg_class, pg_attribute, pg_type
WHERE	pg_class.oid = attrelid AND
	atttypid = pg_type.oid AND
	attlen != typlen;

-- check alignment
SELECT	pg_attribute.oid, relname, attname
FROM	pg_class, pg_attribute, pg_type
WHERE	pg_class.oid = attrelid AND
	atttypid = pg_type.oid AND
	attalign != typalign;

-- check alignment
SELECT	pg_attribute.oid, relname, attname
FROM	pg_class, pg_attribute, pg_type
WHERE	pg_class.oid = attrelid AND
	atttypid = pg_type.oid AND
	attbyval != typbyval;



