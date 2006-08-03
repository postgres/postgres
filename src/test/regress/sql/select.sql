--
-- SELECT
--

-- btree index
-- awk '{if($1<10){print;}else{next;}}' onek.data | sort +0n -1
--
SELECT * FROM onek
   WHERE onek.unique1 < 10
   ORDER BY onek.unique1;

--
-- awk '{if($1<20){print $1,$14;}else{next;}}' onek.data | sort +0nr -1
--
SELECT onek.unique1, onek.stringu1 FROM onek
   WHERE onek.unique1 < 20 
   ORDER BY unique1 using >;

--
-- awk '{if($1>980){print $1,$14;}else{next;}}' onek.data | sort +1d -2
--
SELECT onek.unique1, onek.stringu1 FROM onek
   WHERE onek.unique1 > 980 
   ORDER BY stringu1 using <;
	
--
-- awk '{if($1>980){print $1,$16;}else{next;}}' onek.data |
-- sort +1d -2 +0nr -1
--
SELECT onek.unique1, onek.string4 FROM onek
   WHERE onek.unique1 > 980 
   ORDER BY string4 using <, unique1 using >;
	
--
-- awk '{if($1>980){print $1,$16;}else{next;}}' onek.data |
-- sort +1dr -2 +0n -1
--
SELECT onek.unique1, onek.string4 FROM onek
   WHERE onek.unique1 > 980
   ORDER BY string4 using >, unique1 using <;
	
--
-- awk '{if($1<20){print $1,$16;}else{next;}}' onek.data |
-- sort +0nr -1 +1d -2
--
SELECT onek.unique1, onek.string4 FROM onek
   WHERE onek.unique1 < 20
   ORDER BY unique1 using >, string4 using <;

--
-- awk '{if($1<20){print $1,$16;}else{next;}}' onek.data |
-- sort +0n -1 +1dr -2
--
SELECT onek.unique1, onek.string4 FROM onek
   WHERE onek.unique1 < 20 
   ORDER BY unique1 using <, string4 using >;

--
-- test partial btree indexes
--
-- As of 7.2, planner probably won't pick an indexscan without stats,
-- so ANALYZE first.
--
ANALYZE onek2;

--
-- awk '{if($1<10){print $0;}else{next;}}' onek.data | sort +0n -1
--
SELECT onek2.* FROM onek2 WHERE onek2.unique1 < 10;

--
-- awk '{if($1<20){print $1,$14;}else{next;}}' onek.data | sort +0nr -1
--
SELECT onek2.unique1, onek2.stringu1 FROM onek2
    WHERE onek2.unique1 < 20 
    ORDER BY unique1 using >;

--
-- awk '{if($1>980){print $1,$14;}else{next;}}' onek.data | sort +1d -2
--
SELECT onek2.unique1, onek2.stringu1 FROM onek2
   WHERE onek2.unique1 > 980;


SELECT two, stringu1, ten, string4
   INTO TABLE tmp
   FROM onek;

--
-- awk '{print $1,$2;}' person.data |
-- awk '{if(NF!=2){print $3,$2;}else{print;}}' - emp.data |
-- awk '{if(NF!=2){print $3,$2;}else{print;}}' - student.data |
-- awk 'BEGIN{FS="      ";}{if(NF!=2){print $4,$5;}else{print;}}' - stud_emp.data
--
-- SELECT name, age FROM person*; ??? check if different
SELECT p.name, p.age FROM person* p;

--
-- awk '{print $1,$2;}' person.data |
-- awk '{if(NF!=2){print $3,$2;}else{print;}}' - emp.data |
-- awk '{if(NF!=2){print $3,$2;}else{print;}}' - student.data |
-- awk 'BEGIN{FS="      ";}{if(NF!=1){print $4,$5;}else{print;}}' - stud_emp.data |
-- sort +1nr -2
--
SELECT p.name, p.age FROM person* p ORDER BY age using >, name;

--
-- Test some cases involving whole-row Var referencing a subquery
--
select foo from (select 1) as foo;
select foo from (select null) as foo;
select foo from (select 'xyzzy',1,null) as foo;

--
-- Test VALUES lists
--
select * from onek, (values(147, 'RFAAAA'), (931, 'VJAAAA')) as v (i, j)
    WHERE onek.unique1 = v.i and onek.stringu1 = v.j;

-- a more complex case
-- looks like we're coding lisp :-)
select * from onek,
  (values ((select i from
    (values(10000), (2), (389), (1000), (2000), ((select 10029))) as foo(i)
    order by i asc limit 1))) bar (i)
  where onek.unique1 = bar.i;

-- try VALUES in a subquery
select * from onek
    where (unique1,ten) in (values (1,1), (20,0), (99,9), (17,99))
    order by unique1;

-- VALUES is also legal as a standalone query or a set-operation member
VALUES (1,2), (3,4+4), (7,77.7);

VALUES (1,2), (3,4+4), (7,77.7)
UNION ALL
SELECT 2+2, 57
UNION ALL
SELECT * FROM int8_tbl;
