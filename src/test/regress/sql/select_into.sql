SELECT *
   INTO TABLE temp1
   FROM temp
   WHERE onek.unique1 < 2;

DROP TABLE temp1;

SELECT *
   INTO TABLE temp1
   FROM temp
   WHERE onek2.unique1 < 2;

DROP TABLE temp1;

