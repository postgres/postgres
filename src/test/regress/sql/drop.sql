--
-- drop.source
--

--
-- this will fail if the user is not the postgres superuser.
-- if it does, don't worry about it (you can turn usersuper
-- back on as "postgres").  too many people don't follow 
-- directions and run this as "postgres", though...
--
UPDATE pg_user
   SET usesuper = 't'::bool
   WHERE usename = 'postgres';


--
-- FUNCTION REMOVAL
--
DROP FUNCTION hobbies(person);

DROP FUNCTION hobby_construct(text,text);

DROP FUNCTION equipment(hobbies_r);

DROP FUNCTION user_relns();

DROP FUNCTION widget_in(opaque);

DROP FUNCTION widget_out(opaque);

DROP FUNCTION pt_in_widget(point,widget);

DROP FUNCTION overpaid(emp);

DROP FUNCTION boxarea(box);

DROP FUNCTION interpt_pp(path,path);

DROP FUNCTION reverse_name(name);


--
-- OPERATOR REMOVAL
--
DROP OPERATOR ## (path, path);

DROP OPERATOR <% (point, widget);

-- left unary 
DROP OPERATOR @#@ (none, int4);

-- right unary 
DROP OPERATOR #@# (int4, none);	

-- right unary 
DROP OPERATOR #%# (int4, none);	


--
-- ABSTRACT DATA TYPE REMOVAL
--
DROP TYPE city_budget;

DROP TYPE widget;


--
-- RULE REMOVAL
--	(is also tested in queries.source)
--

--
-- AGGREGATE REMOVAL
--
DROP AGGREGATE newavg int4;

DROP AGGREGATE newsum int4;

DROP AGGREGATE newcnt int4;


--
-- CLASS REMOVAL
--	(inheritance hierarchies are deleted in reverse order)
--

--
-- DROP ancillary data structures (i.e. indices)
--
DROP INDEX onek_unique1;

DROP INDEX onek_unique2;

DROP INDEX onek_hundred;

DROP INDEX onek_stringu1;

DROP INDEX tenk1_unique1;

DROP INDEX tenk1_unique2;

DROP INDEX tenk1_hundred;

DROP INDEX tenk2_unique1;

DROP INDEX tenk2_unique2;

DROP INDEX tenk2_hundred;

-- DROP INDEX onek2_u1_prtl;

-- DROP INDEX onek2_u2_prtl;

-- DROP INDEX onek2_stu1_prtl;

DROP INDEX rect2ind;

DROP INDEX rix;

DROP INDEX iix;

DROP INDEX six;

DROP INDEX hash_i4_index;

DROP INDEX hash_name_index;

DROP INDEX hash_txt_index;

DROP INDEX hash_f8_index;

-- DROP INDEX hash_ovfl_index;

DROP INDEX bt_i4_index;

DROP INDEX bt_name_index;

DROP INDEX bt_txt_index;

DROP INDEX bt_f8_index;


DROP TABLE  onek;

DROP TABLE  onek2;

DROP TABLE  tenk1;

DROP TABLE  tenk2;

DROP TABLE  Bprime;


DROP TABLE  hobbies_r;

DROP TABLE  equipment_r;


DROP TABLE  aggtest;

DROP TABLE  xacttest;

DROP TABLE  arrtest;

DROP TABLE  iportaltest;


DROP TABLE  f_star;

DROP TABLE  e_star;

DROP TABLE  d_star;

DROP TABLE  c_star;

DROP TABLE  b_star;

DROP TABLE  a_star;


--
-- must be in reverse inheritance order
--
DROP TABLE  stud_emp;

DROP TABLE  student;

DROP TABLE  slow_emp4000;

DROP TABLE  fast_emp4000;

DROP TABLE  emp;

DROP TABLE  person;


DROP TABLE  ramp;

DROP TABLE  real_city;

DROP TABLE  dept;

DROP TABLE  ihighway;

DROP TABLE  shighway;

DROP TABLE  road;

DROP TABLE  city;


DROP TABLE  hash_i4_heap;

DROP TABLE  hash_name_heap;

DROP TABLE  hash_txt_heap;

DROP TABLE  hash_f8_heap;

-- DROP TABLE  hash_ovfl_heap;

DROP TABLE  bt_i4_heap;

DROP TABLE  bt_name_heap;

DROP TABLE  bt_txt_heap;

DROP TABLE  bt_f8_heap;


DROP TABLE  ABSTIME_TBL;

DROP TABLE  RELTIME_TBL;

DROP TABLE  TINTERVAL_TBL;

--
-- VIRTUAL CLASS REMOVAL
--	(also tests removal of rewrite rules)
--
DROP VIEW street;

DROP VIEW iexit;

DROP VIEW toyemp;

