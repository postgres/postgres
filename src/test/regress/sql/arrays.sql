--
-- ARRAYS
--

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
