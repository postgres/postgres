--
-- test the random function
--
-- count the number of tuples originally
SELECT count(*) FROM onek;

-- select roughly 1/10 of the tuples
SELECT count(*) FROM onek where oidrand(onek.oid, 10);

-- select again, the count should be different
SELECT count(*) FROM onek where oidrand(onek.oid, 10);

