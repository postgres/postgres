--
-- Test data type behavior
--

--
-- Base/common types
--

CREATE FUNCTION test_type_conversion_bool(x bool) RETURNS bool AS $$
plpy.info(x, type(x))
return x
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_bool(true);
SELECT * FROM test_type_conversion_bool(false);
SELECT * FROM test_type_conversion_bool(null);


-- test various other ways to express Booleans in Python
CREATE FUNCTION test_type_conversion_bool_other(n int) RETURNS bool AS $$
# numbers
if n == 0:
   ret = 0
elif n == 1:
   ret = 5
# strings
elif n == 2:
   ret = ''
elif n == 3:
   ret = 'fa' # true in Python, false in PostgreSQL
# containers
elif n == 4:
   ret = []
elif n == 5:
   ret = [0]
plpy.info(ret, not not ret)
return ret
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_bool_other(0);
SELECT * FROM test_type_conversion_bool_other(1);
SELECT * FROM test_type_conversion_bool_other(2);
SELECT * FROM test_type_conversion_bool_other(3);
SELECT * FROM test_type_conversion_bool_other(4);
SELECT * FROM test_type_conversion_bool_other(5);


CREATE FUNCTION test_type_conversion_char(x char) RETURNS char AS $$
plpy.info(x, type(x))
return x
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_char('a');
SELECT * FROM test_type_conversion_char(null);


CREATE FUNCTION test_type_conversion_int2(x int2) RETURNS int2 AS $$
plpy.info(x, type(x))
return x
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_int2(100::int2);
SELECT * FROM test_type_conversion_int2(-100::int2);
SELECT * FROM test_type_conversion_int2(null);


CREATE FUNCTION test_type_conversion_int4(x int4) RETURNS int4 AS $$
plpy.info(x, type(x))
return x
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_int4(100);
SELECT * FROM test_type_conversion_int4(-100);
SELECT * FROM test_type_conversion_int4(null);


CREATE FUNCTION test_type_conversion_int8(x int8) RETURNS int8 AS $$
plpy.info(x, type(x))
return x
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_int8(100);
SELECT * FROM test_type_conversion_int8(-100);
SELECT * FROM test_type_conversion_int8(5000000000);
SELECT * FROM test_type_conversion_int8(null);


CREATE FUNCTION test_type_conversion_numeric(x numeric) RETURNS numeric AS $$
plpy.info(x, type(x))
return x
$$ LANGUAGE plpythonu;

/* The current implementation converts numeric to float. */
SELECT * FROM test_type_conversion_numeric(100);
SELECT * FROM test_type_conversion_numeric(-100);
SELECT * FROM test_type_conversion_numeric(5000000000.5);
SELECT * FROM test_type_conversion_numeric(null);


CREATE FUNCTION test_type_conversion_float4(x float4) RETURNS float4 AS $$
plpy.info(x, type(x))
return x
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_float4(100);
SELECT * FROM test_type_conversion_float4(-100);
SELECT * FROM test_type_conversion_float4(5000.5);
SELECT * FROM test_type_conversion_float4(null);


CREATE FUNCTION test_type_conversion_float8(x float8) RETURNS float8 AS $$
plpy.info(x, type(x))
return x
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_float8(100);
SELECT * FROM test_type_conversion_float8(-100);
SELECT * FROM test_type_conversion_float8(5000000000.5);
SELECT * FROM test_type_conversion_float8(null);


CREATE FUNCTION test_type_conversion_text(x text) RETURNS text AS $$
plpy.info(x, type(x))
return x
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_text('hello world');
SELECT * FROM test_type_conversion_text(null);


CREATE FUNCTION test_type_conversion_bytea(x bytea) RETURNS bytea AS $$
plpy.info(x, type(x))
return x
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_bytea('hello world');
SELECT * FROM test_type_conversion_bytea(E'null\\000byte');
SELECT * FROM test_type_conversion_bytea(null);


CREATE FUNCTION test_type_marshal() RETURNS bytea AS $$
import marshal
return marshal.dumps('hello world')
$$ LANGUAGE plpythonu;

CREATE FUNCTION test_type_unmarshal(x bytea) RETURNS text AS $$
import marshal
try:
    return marshal.loads(x)
except ValueError, e:
    return 'FAILED: ' + str(e)
$$ LANGUAGE plpythonu;

SELECT test_type_unmarshal(x) FROM test_type_marshal() x;


--
-- Domains
--

CREATE DOMAIN booltrue AS bool CHECK (VALUE IS TRUE OR VALUE IS NULL);

CREATE FUNCTION test_type_conversion_booltrue(x booltrue, y bool) RETURNS booltrue AS $$
return y
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_booltrue(true, true);
SELECT * FROM test_type_conversion_booltrue(false, true);
SELECT * FROM test_type_conversion_booltrue(true, false);


CREATE DOMAIN uint2 AS int2 CHECK (VALUE >= 0);

CREATE FUNCTION test_type_conversion_uint2(x uint2, y int) RETURNS uint2 AS $$
plpy.info(x, type(x))
return y
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_uint2(100::uint2, 50);
SELECT * FROM test_type_conversion_uint2(100::uint2, -50);
SELECT * FROM test_type_conversion_uint2(null, 1);


CREATE DOMAIN nnint AS int CHECK (VALUE IS NOT NULL);

CREATE FUNCTION test_type_conversion_nnint(x nnint, y int) RETURNS nnint AS $$
return y
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_nnint(10, 20);
SELECT * FROM test_type_conversion_nnint(null, 20);
SELECT * FROM test_type_conversion_nnint(10, null);


CREATE DOMAIN bytea10 AS bytea CHECK (octet_length(VALUE) = 10 AND VALUE IS NOT NULL);

CREATE FUNCTION test_type_conversion_bytea10(x bytea10, y bytea) RETURNS bytea10 AS $$
plpy.info(x, type(x))
return y
$$ LANGUAGE plpythonu;

SELECT * FROM test_type_conversion_bytea10('hello wold', 'hello wold');
SELECT * FROM test_type_conversion_bytea10('hello world', 'hello wold');
SELECT * FROM test_type_conversion_bytea10('hello word', 'hello world');
SELECT * FROM test_type_conversion_bytea10(null, 'hello word');
SELECT * FROM test_type_conversion_bytea10('hello word', null);
