--
-- MISC
--

-- directory paths and dlsuffix are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR
\getenv abs_builddir PG_ABS_BUILDDIR
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION overpaid(emp)
   RETURNS bool
   AS :'regresslib'
   LANGUAGE C STRICT;

CREATE FUNCTION reverse_name(name)
   RETURNS name
   AS :'regresslib'
   LANGUAGE C STRICT;

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
SELECT two, stringu1, ten, string4
   INTO TABLE tmp
   FROM onek;

UPDATE tmp
   SET stringu1 = reverse_name(onek.stringu1)
   FROM onek
   WHERE onek.stringu1 = 'JBAAAA' and
	  onek.stringu1 = tmp.stringu1;

UPDATE tmp
   SET stringu1 = reverse_name(onek2.stringu1)
   FROM onek2
   WHERE onek2.stringu1 = 'JCAAAA' and
	  onek2.stringu1 = tmp.stringu1;

DROP TABLE tmp;

--UPDATE person*
--   SET age = age + 1;

--UPDATE person*
--   SET age = age + 3
--   WHERE name = 'linda';

--
-- copy
--
\set filename :abs_builddir '/results/onek.data'
COPY onek TO :'filename';

CREATE TEMP TABLE onek_copy (LIKE onek);

COPY onek_copy FROM :'filename';

SELECT * FROM onek EXCEPT ALL SELECT * FROM onek_copy;

SELECT * FROM onek_copy EXCEPT ALL SELECT * FROM onek;

\set filename :abs_builddir '/results/stud_emp.data'
COPY BINARY stud_emp TO :'filename';

CREATE TEMP TABLE stud_emp_copy (LIKE stud_emp);

COPY BINARY stud_emp_copy FROM :'filename';

SELECT * FROM stud_emp_copy;

--
-- test data for postquel functions
--

CREATE TABLE hobbies_r (
	name		text,
	person 		text
);

CREATE TABLE equipment_r (
	name 		text,
	hobby		text
);

INSERT INTO hobbies_r (name, person)
   SELECT 'posthacking', p.name
   FROM person* p
   WHERE p.name = 'mike' or p.name = 'jeff';

INSERT INTO hobbies_r (name, person)
   SELECT 'basketball', p.name
   FROM person p
   WHERE p.name = 'joe' or p.name = 'sally';

INSERT INTO hobbies_r (name) VALUES ('skywalking');

INSERT INTO equipment_r (name, hobby) VALUES ('advil', 'posthacking');

INSERT INTO equipment_r (name, hobby) VALUES ('peet''s coffee', 'posthacking');

INSERT INTO equipment_r (name, hobby) VALUES ('hightops', 'basketball');

INSERT INTO equipment_r (name, hobby) VALUES ('guts', 'skywalking');

--
-- postquel functions
--

CREATE FUNCTION hobbies(person)
   RETURNS setof hobbies_r
   AS 'select * from hobbies_r where person = $1.name'
   LANGUAGE SQL;

CREATE FUNCTION hobby_construct(text, text)
   RETURNS hobbies_r
   AS 'select $1 as name, $2 as hobby'
   LANGUAGE SQL;

CREATE FUNCTION hobby_construct_named(name text, hobby text)
   RETURNS hobbies_r
   AS 'select name, hobby'
   LANGUAGE SQL;

CREATE FUNCTION hobbies_by_name(hobbies_r.name%TYPE)
   RETURNS hobbies_r.person%TYPE
   AS 'select person from hobbies_r where name = $1'
   LANGUAGE SQL;

CREATE FUNCTION equipment(hobbies_r)
   RETURNS setof equipment_r
   AS 'select * from equipment_r where hobby = $1.name'
   LANGUAGE SQL;

CREATE FUNCTION equipment_named(hobby hobbies_r)
   RETURNS setof equipment_r
   AS 'select * from equipment_r where equipment_r.hobby = equipment_named.hobby.name'
   LANGUAGE SQL;

CREATE FUNCTION equipment_named_ambiguous_1a(hobby hobbies_r)
   RETURNS setof equipment_r
   AS 'select * from equipment_r where hobby = equipment_named_ambiguous_1a.hobby.name'
   LANGUAGE SQL;

CREATE FUNCTION equipment_named_ambiguous_1b(hobby hobbies_r)
   RETURNS setof equipment_r
   AS 'select * from equipment_r where equipment_r.hobby = hobby.name'
   LANGUAGE SQL;

CREATE FUNCTION equipment_named_ambiguous_1c(hobby hobbies_r)
   RETURNS setof equipment_r
   AS 'select * from equipment_r where hobby = hobby.name'
   LANGUAGE SQL;

CREATE FUNCTION equipment_named_ambiguous_2a(hobby text)
   RETURNS setof equipment_r
   AS 'select * from equipment_r where hobby = equipment_named_ambiguous_2a.hobby'
   LANGUAGE SQL;

CREATE FUNCTION equipment_named_ambiguous_2b(hobby text)
   RETURNS setof equipment_r
   AS 'select * from equipment_r where equipment_r.hobby = hobby'
   LANGUAGE SQL;

--
-- mike does post_hacking,
-- joe and sally play basketball, and
-- everyone else does nothing.
--
SELECT p.name, name(p.hobbies) FROM ONLY person p;

--
-- as above, but jeff also does post_hacking.
--
SELECT p.name, name(p.hobbies) FROM person* p;

--
-- the next two queries demonstrate how functions generate bogus duplicates.
-- this is a "feature" ..
--
SELECT DISTINCT hobbies_r.name, name(hobbies_r.equipment) FROM hobbies_r
  ORDER BY 1,2;

SELECT hobbies_r.name, (hobbies_r.equipment).name FROM hobbies_r;

--
-- mike needs advil and peet's coffee,
-- joe and sally need hightops, and
-- everyone else is fine.
--
SELECT p.name, name(p.hobbies), name(equipment(p.hobbies)) FROM ONLY person p;

--
-- as above, but jeff needs advil and peet's coffee as well.
--
SELECT p.name, name(p.hobbies), name(equipment(p.hobbies)) FROM person* p;

--
-- just like the last two, but make sure that the target list fixup and
-- unflattening is being done correctly.
--
SELECT name(equipment(p.hobbies)), p.name, name(p.hobbies) FROM ONLY person p;

SELECT (p.hobbies).equipment.name, p.name, name(p.hobbies) FROM person* p;

SELECT (p.hobbies).equipment.name, name(p.hobbies), p.name FROM ONLY person p;

SELECT name(equipment(p.hobbies)), name(p.hobbies), p.name FROM person* p;

SELECT name(equipment(hobby_construct(text 'skywalking', text 'mer')));

SELECT name(equipment(hobby_construct_named(text 'skywalking', text 'mer')));

SELECT name(equipment_named(hobby_construct_named(text 'skywalking', text 'mer')));

SELECT name(equipment_named_ambiguous_1a(hobby_construct_named(text 'skywalking', text 'mer')));

SELECT name(equipment_named_ambiguous_1b(hobby_construct_named(text 'skywalking', text 'mer')));

SELECT name(equipment_named_ambiguous_1c(hobby_construct_named(text 'skywalking', text 'mer')));

SELECT name(equipment_named_ambiguous_2a(text 'skywalking'));

SELECT name(equipment_named_ambiguous_2b(text 'skywalking'));

SELECT hobbies_by_name('basketball');

SELECT name, overpaid(emp.*) FROM emp;

--
-- Try a few cases with SQL-spec row constructor expressions
--
SELECT * FROM equipment(ROW('skywalking', 'mer'));

SELECT name(equipment(ROW('skywalking', 'mer')));

SELECT *, name(equipment(h.*)) FROM hobbies_r h;

SELECT *, (equipment(CAST((h.*) AS hobbies_r))).name FROM hobbies_r h;

--
-- functional joins
--

--
-- instance rules
--

--
-- rewrite rules
--
