--
-- replace
--
--
-- BTREE
--
UPDATE onek
   SET unique1 = onek.unique1 + 1;

UPDATE onek
   SET unique1 = onek.unique1 - 1;

--
-- BTREE partial
--
-- UPDATE onek2
--   SET unique1 = onek2.unique1 + 1;

--UPDATE onek2 
--   SET unique1 = onek2.unique1 - 1;

--
-- BTREE shutting out non-functional updates
--
-- the following two tests seem to take a long time on some 
-- systems.    This non-func update stuff needs to be examined
-- more closely.  			- jolly (2/22/96)
-- 
UPDATE temp
   SET stringu1 = reverse_c16(onek.stringu1)
   WHERE onek.stringu1 = 'JBAAAA' and
	  onek.stringu1 = temp.stringu1;

UPDATE temp
   SET stringu1 = reverse_c16(onek2.stringu1)
   WHERE onek2.stringu1 = 'JCAAAA' and
	  onek2.stringu1 = temp.stringu1;

DROP TABLE temp;

--UPDATE person*
--   SET age = age + 1;

--UPDATE person*
--   SET age = age + 3
--   WHERE name = 'linda';

--
-- copy
--
COPY onek TO '_OBJWD_/results/onek.data';

DELETE FROM onek;

COPY onek FROM '_OBJWD_/results/onek.data';

SELECT unique1 FROM onek WHERE unique1 < 2;

DELETE FROM onek2;

COPY onek2 FROM '_OBJWD_/results/onek.data';

SELECT unique1 FROM onek2 WHERE unique1 < 2;

COPY BINARY stud_emp TO '_OBJWD_/results/stud_emp.data';

DELETE FROM stud_emp;

COPY BINARY stud_emp FROM '_OBJWD_/results/stud_emp.data';

SELECT * FROM stud_emp;

-- COPY aggtest FROM stdin;
-- 56	7.8
-- 100	99.097
-- 0	0.09561
-- 42	324.78
-- .
-- COPY aggtest TO stdout;


--
-- test the random function
--
-- count the number of tuples originally
SELECT count(*) FROM onek;

-- select roughly 1/10 of the tuples
SELECT count(*) FROM onek where oidrand(onek.oid, 10);

-- select again, the count should be different
SELECT count(*) FROM onek where oidrand(onek.oid, 10);

--
-- AGGREGATES
--
SELECT avg(four) AS avg_1 FROM onek;

SELECT avg(a) AS avg_49 FROM aggtest WHERE a < 100;

SELECT avg(b) AS avg_107_943 FROM aggtest;

SELECT avg(gpa) AS avg_3_4 FROM student;


SELECT sum(four) AS sum_1500 FROM onek;

SELECT sum(a) AS sum_198 FROM aggtest;

SELECT sum(b) AS avg_431_773 FROM aggtest;

SELECT sum(gpa) AS avg_6_8 FROM student;


SELECT max(four) AS max_3 FROM onek;

SELECT max(a) AS max_100 FROM aggtest;

SELECT max(aggtest.b) AS max_324_78 FROM aggtest;

SELECT max(student.gpa) AS max_3_7 FROM student;


SELECT count(four) AS cnt_1000 FROM onek;


SELECT newavg(four) AS avg_1 FROM onek;

SELECT newsum(four) AS sum_1500 FROM onek;

SELECT newcnt(four) AS cnt_1000 FROM onek;

