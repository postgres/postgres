--
-- num_nulls()
--

SELECT num_nonnulls(NULL);
SELECT num_nonnulls('1');
SELECT num_nonnulls(NULL::text);
SELECT num_nonnulls(NULL::text, NULL::int);
SELECT num_nonnulls(1, 2, NULL::text, NULL::point, '', int8 '9', 1.0 / NULL);
SELECT num_nonnulls(VARIADIC '{1,2,NULL,3}'::int[]);
SELECT num_nonnulls(VARIADIC '{"1","2","3","4"}'::text[]);
SELECT num_nonnulls(VARIADIC ARRAY(SELECT CASE WHEN i <> 40 THEN i END FROM generate_series(1, 100) i));

SELECT num_nulls(NULL);
SELECT num_nulls('1');
SELECT num_nulls(NULL::text);
SELECT num_nulls(NULL::text, NULL::int);
SELECT num_nulls(1, 2, NULL::text, NULL::point, '', int8 '9', 1.0 / NULL);
SELECT num_nulls(VARIADIC '{1,2,NULL,3}'::int[]);
SELECT num_nulls(VARIADIC '{"1","2","3","4"}'::text[]);
SELECT num_nulls(VARIADIC ARRAY(SELECT CASE WHEN i <> 40 THEN i END FROM generate_series(1, 100) i));

-- special cases
SELECT num_nonnulls(VARIADIC NULL::text[]);
SELECT num_nonnulls(VARIADIC '{}'::int[]);
SELECT num_nulls(VARIADIC NULL::text[]);
SELECT num_nulls(VARIADIC '{}'::int[]);

-- should fail, one or more arguments is required
SELECT num_nonnulls();
SELECT num_nulls();
