--
-- ARRAYS
--

CREATE TABLE arrtest (
	a 			int2[],
	b 			int4[][][],
	c 			name[],
	d			text[][], 
	e 			float8[],
	f			char(5)[],
	g			varchar(5)[]
);

--
-- only this array as a 0-based 'e', the others are 1-based.
-- 'e' is also a large object.
--

INSERT INTO arrtest (a[5], b[2][1][2], c, d, f, g)
   VALUES ('{1,2,3,4,5}', '{{{},{1,2}}}', '{}', '{}', '{}', '{}');

UPDATE arrtest SET e[0] = '1.1';

UPDATE arrtest SET e[1] = '2.2';

INSERT INTO arrtest (f)
   VALUES ('{"too long"}');

INSERT INTO arrtest (a, b[2][2][1], c, d, e, f, g)
   VALUES ('{11,12,23}', '{{3,4},{4,5}}', '{"foobar"}', 
           '{{"elt1", "elt2"}}', '{"3.4", "6.7"}',
           '{"abc","abcde"}', '{"abc","abcde"}');

INSERT INTO arrtest (a, b[1][2][2], c, d[2][1])
   VALUES ('{}', '{3,4}', '{foo,bar}', '{bar,foo}');


SELECT * FROM arrtest;

SELECT arrtest.a[1],
          arrtest.b[1][1][1],
          arrtest.c[1],
          arrtest.d[1][1], 
          arrtest.e[0]
   FROM arrtest;

SELECT a[1], b[1][1][1], c[1], d[1][1], e[0]
   FROM arrtest;

SELECT a[1:3],
          b[1:1][1:2][1:2],
          c[1:2], 
          d[1:1][1:2]
   FROM arrtest;

SELECT array_dims(a) AS a,array_dims(b) AS b,array_dims(c) AS c
   FROM arrtest;

-- returns nothing 
SELECT *
   FROM arrtest
   WHERE a[1] < 5 and 
         c = '{"foobar"}'::_name;

UPDATE arrtest
  SET a[1:2] = '{16,25}'
  WHERE NOT a = '{}'::_int2;

UPDATE arrtest
  SET b[1:1][1:1][1:2] = '{113, 117}',
      b[1:1][1:2][2:2] = '{142, 147}'
  WHERE array_dims(b) = '[1:1][1:2][1:2]';

UPDATE arrtest
  SET c[2:2] = '{"new_word"}'
  WHERE array_dims(c) is not null;

SELECT a,b,c FROM arrtest;

SELECT a[1:3],
          b[1:1][1:2][1:2],
          c[1:2], 
          d[1:1][2:2]
   FROM arrtest;
