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

-- returns three different results--
SELECT array_dims(arrtest.b) AS x;

-- returns nothing 
SELECT *
   FROM arrtest
   WHERE a[1] < 5 and 
         c = '{"foobar"}'::_name;

UPDATE arrtest
  SET a[1:2] = '{16,25}',
      b[1:1][1:1][1:2] = '{113, 117}', 
      c[1:1] = '{"new_word"}';

SELECT a[1:3],
          b[1:1][1:2][1:2],
          c[1:2], 
          d[1:1][2:2]
   FROM arrtest;
