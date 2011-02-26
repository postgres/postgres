CREATE FUNCTION multiout_simple(OUT i integer, OUT j integer) AS $$
return (1, 2)
$$ LANGUAGE plpythonu;

SELECT multiout_simple();
SELECT * FROM multiout_simple();
SELECT i, j + 2 FROM multiout_simple();
SELECT (multiout_simple()).j + 3;

CREATE FUNCTION multiout_simple_setof(n integer = 1, OUT integer, OUT integer) RETURNS SETOF record AS $$
return [(1, 2)] * n
$$ LANGUAGE plpythonu;

SELECT multiout_simple_setof();
SELECT * FROM multiout_simple_setof();
SELECT * FROM multiout_simple_setof(3);

CREATE FUNCTION multiout_record_as(typ text,
                                   first text, OUT first text,
                                   second integer, OUT second integer,
                                   retnull boolean) RETURNS record AS $$
if retnull:
    return None
if typ == 'dict':
    return { 'first': first, 'second': second, 'additionalfield': 'must not cause trouble' }
elif typ == 'tuple':
    return ( first, second )
elif typ == 'list':
    return [ first, second ]
elif typ == 'obj':
    class type_record: pass
    type_record.first = first
    type_record.second = second
    return type_record
$$ LANGUAGE plpythonu;

SELECT * FROM multiout_record_as('dict', 'foo', 1, 'f');
SELECT multiout_record_as('dict', 'foo', 1, 'f');
SELECT *, s IS NULL AS snull FROM multiout_record_as('tuple', 'xxx', NULL, 'f') AS f(f, s);
SELECT *, f IS NULL AS fnull, s IS NULL AS snull FROM multiout_record_as('tuple', 'xxx', 1, 't') AS f(f, s);
SELECT * FROM multiout_record_as('obj', NULL, 10, 'f');

CREATE FUNCTION multiout_setof(n integer,
                               OUT power_of_2 integer,
                               OUT length integer) RETURNS SETOF record AS $$
for i in range(n):
    power = 2 ** i
    length = plpy.execute("select length('%d')" % power)[0]['length']
    yield power, length
$$ LANGUAGE plpythonu;

SELECT * FROM multiout_setof(3);
SELECT multiout_setof(5);

CREATE FUNCTION multiout_return_table() RETURNS TABLE (x integer, y text) AS $$
return [{'x': 4, 'y' :'four'},
        {'x': 7, 'y' :'seven'},
        {'x': 0, 'y' :'zero'}]
$$ LANGUAGE plpythonu;

SELECT * FROM multiout_return_table();

CREATE FUNCTION multiout_array(OUT integer[], OUT text) RETURNS SETOF record AS $$
yield [[1], 'a']
yield [[1,2], 'b']
yield [[1,2,3], None]
$$ LANGUAGE plpythonu;

SELECT * FROM multiout_array();

CREATE FUNCTION singleout_composite(OUT type_record) AS $$
return {'first': 1, 'second': 2}
$$ LANGUAGE plpythonu;

CREATE FUNCTION multiout_composite(OUT type_record) RETURNS SETOF type_record AS $$
return [{'first': 1, 'second': 2},
       {'first': 3, 'second': 4	}]
$$ LANGUAGE plpythonu;

SELECT * FROM singleout_composite();
SELECT * FROM multiout_composite();

-- composite OUT parameters in functions returning RECORD not supported yet
CREATE FUNCTION multiout_composite(INOUT n integer, OUT type_record) AS $$
return (n, (n * 2, n * 3))
$$ LANGUAGE plpythonu;

CREATE FUNCTION multiout_table_type_setof(typ text, returnnull boolean, INOUT n integer, OUT table_record) RETURNS SETOF record AS $$
if returnnull:
    d = None
elif typ == 'dict':
    d = {'first': n * 2, 'second': n * 3, 'extra': 'not important'}
elif typ == 'tuple':
    d = (n * 2, n * 3)
elif typ == 'obj':
    class d: pass
    d.first = n * 2
    d.second = n * 3
for i in range(n):
    yield (i, d)
$$ LANGUAGE plpythonu;

SELECT * FROM multiout_composite(2);
SELECT * FROM multiout_table_type_setof('dict', 'f', 3);
SELECT * FROM multiout_table_type_setof('tuple', 'f', 2);
SELECT * FROM multiout_table_type_setof('obj', 'f', 4);
SELECT * FROM multiout_table_type_setof('dict', 't', 3);

-- check what happens if a type changes under us

CREATE TABLE changing (
    i integer,
    j integer
);

CREATE FUNCTION changing_test(OUT n integer, OUT changing) RETURNS SETOF record AS $$
return [(1, {'i': 1, 'j': 2}),
        (1, (3, 4))]
$$ LANGUAGE plpythonu;

SELECT * FROM changing_test();
ALTER TABLE changing DROP COLUMN j;
SELECT * FROM changing_test();
SELECT * FROM changing_test();
ALTER TABLE changing ADD COLUMN j integer;
SELECT * FROM changing_test();

-- tables of composite types (not yet implemented)

CREATE FUNCTION composite_types_table(OUT tab table_record[], OUT typ type_record[] ) RETURNS SETOF record AS $$
yield {'tab': [['first', 1], ['second', 2]],
      'typ': [{'first': 'third', 'second': 3},
              {'first': 'fourth', 'second': 4}]}
yield {'tab': [['first', 1], ['second', 2]],
      'typ': [{'first': 'third', 'second': 3},
              {'first': 'fourth', 'second': 4}]}
yield {'tab': [['first', 1], ['second', 2]],
      'typ': [{'first': 'third', 'second': 3},
              {'first': 'fourth', 'second': 4}]}
$$ LANGUAGE plpythonu;

SELECT * FROM composite_types_table();

-- check what happens if the output record descriptor changes
CREATE FUNCTION return_record(t text) RETURNS record AS $$
return {'t': t, 'val': 10}
$$ LANGUAGE plpythonu;

SELECT * FROM return_record('abc') AS r(t text, val integer);
SELECT * FROM return_record('abc') AS r(t text, val bigint);
SELECT * FROM return_record('abc') AS r(t text, val integer);
SELECT * FROM return_record('abc') AS r(t varchar(30), val integer);
SELECT * FROM return_record('abc') AS r(t varchar(100), val integer);
SELECT * FROM return_record('999') AS r(val text, t integer);

CREATE FUNCTION return_record_2(t text) RETURNS record AS $$
return {'v1':1,'v2':2,t:3}
$$ LANGUAGE plpythonu;

SELECT * FROM return_record_2('v3') AS (v3 int, v2 int, v1 int);
SELECT * FROM return_record_2('v3') AS (v2 int, v3 int, v1 int);
SELECT * FROM return_record_2('v4') AS (v1 int, v4 int, v2 int);
SELECT * FROM return_record_2('v4') AS (v1 int, v4 int, v2 int);
-- error
SELECT * FROM return_record_2('v4') AS (v1 int, v3 int, v2 int);
-- works
SELECT * FROM return_record_2('v3') AS (v1 int, v3 int, v2 int);
SELECT * FROM return_record_2('v3') AS (v1 int, v2 int, v3 int);
