--
-- SELECT_INTO
--

SELECT *
   INTO TABLE tmp1
   FROM tmp
   WHERE onek.unique1 < 2;

DROP TABLE tmp1;

SELECT *
   INTO TABLE tmp1
   FROM tmp
   WHERE onek2.unique1 < 2;

DROP TABLE tmp1;

