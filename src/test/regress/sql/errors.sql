--
-- ERRORS
--

-- bad in postquel, but ok in postsql
select 1


--
-- UNSUPPORTED STUFF
 
-- doesn't work 
-- attachas nonesuch
--
-- doesn't work 
-- notify pg_class
--

--
-- RETRIEVE
 
-- missing relation name 
select

-- no such relation 
select * from nonesuch;

-- bad name in target list
select nonesuch from pg_database;
-- bad attribute name on lhs of operator
select * from pg_database where nonesuch = pg_database.datname;

-- bad attribute name on rhs of operator
select * from pg_database where pg_database.datname = nonesuch;


-- bad select distinct on syntax, distinct attribute missing
select distinct on (foobar) from pg_database;


-- bad select distinct on syntax, distinct attribute not in target list
select distinct on (foobar) * from pg_database;


--
-- DELETE
 
-- missing relation name (this had better not wildcard!) 
delete from;

-- no such relation 
delete from nonesuch;


--
-- DESTROY
 
-- missing relation name (this had better not wildcard!) 
drop table;

-- no such relation 
drop table nonesuch;


--
-- RENAME
 

-- relation renaming 

-- missing relation name 
alter table rename;

-- no such relation 
alter table nonesuch rename to newnonesuch;

-- no such relation 
alter table nonesuch rename to stud_emp;

-- system relation 
alter table stud_emp rename to pg_stud_emp;

-- conflict 
alter table stud_emp rename to aggtest;

-- self-conflict 
alter table stud_emp rename to stud_emp;


-- attribute renaming 

-- no such relation 
alter table nonesuchrel rename column nonesuchatt to newnonesuchatt;

-- no such attribute 
alter table emp rename column nonesuchatt to newnonesuchatt;

-- conflict 
alter table emp rename column salary to manager;

-- conflict 
alter table emp rename column salary to oid;


--
-- TRANSACTION STUFF
 
-- not in a xact 
abort;

-- not in a xact 
end;


--
-- DEFINE AGGREGATE
 
-- left out finalfunc 
create aggregate newavg1 (sfunc1 = int4pl,
			  basetype = int4,
			  stype1 = int4,
			  sfunc2 = int4inc,
			  stype2 = int4, 
			  initcond1 = '0',
			  initcond2 = '0');

-- sfunc return type disagreement 
create aggregate newavg2 (sfunc1 = int4pl,
			  basetype = int4,
			  stype1 = int4,
			  sfunc2 = int2inc,
			  stype2 = int2,
			  finalfunc = int4div,
			  initcond1 = '0',
			  initcond2 = '0');

-- sfunc/finalfunc type disagreement 
create aggregate newavg3 (sfunc1 = int4pl,
			  basetype = int4,
			  stype1 = int4,
			  sfunc2 = int4inc,
			  stype2 = int4,
			  finalfunc = int2div,
			  initcond1 = '0',
			  initcond2 = '0');

-- left out basetype
create aggregate newcnt1 (sfunc2 = int4inc,
			  stype2 = int4,
			initcond2 = '0');

-- left out initcond2 (for sfunc2) 
create aggregate newcnt1 (sfunc2 = int4inc,
			  basetype = int4,
			  stype2 = int4);


--
-- REMOVE INDEX
 
-- missing index name 
drop index;

-- bad index name 
drop index 314159;

-- no such index 
drop index nonesuch;


--
-- REMOVE AGGREGATE
 
-- missing aggregate name 
drop aggregate;

-- bad aggregate name 
drop aggregate 314159;

-- no such aggregate 
drop aggregate nonesuch;

-- missing aggregate type
drop aggregate newcnt1;

-- bad aggregate type
drop aggregate newcnt nonesuch;

-- no such aggregate for type
drop aggregate newcnt float4;


--
-- REMOVE FUNCTION
 
-- missing function name 
drop function ();

-- bad function name 
drop function 314159();

-- no such function 
drop function nonesuch();


--
-- REMOVE TYPE
 
-- missing type name 
drop type;

-- bad type name 
drop type 314159;

-- no such type 
drop type nonesuch;


--
-- DROP OPERATOR
 
-- missing everything 
drop operator;

-- bad operator name 
drop operator equals;

-- missing type list 
drop operator ===;

-- missing parentheses 
drop operator int4, int4;

-- missing operator name 
drop operator (int4, int4);

-- missing type list contents 
drop operator === ();

-- no such operator 
drop operator === (int4);

-- no such operator by that name 
drop operator === (int4, int4);

-- no such type1 
drop operator = (nonesuch);

-- no such type1 
drop operator = ( , int4);

-- no such type1 
drop operator = (nonesuch, int4);

-- no such type2 
drop operator = (int4, nonesuch);

-- no such type2 
drop operator = (int4, );


--
-- DROP RULE
 
-- missing rule name 
drop rule;

-- bad rule name 
drop rule 314159;

-- no such rule 
drop rule nonesuch;

-- bad keyword 
drop tuple rule nonesuch;

-- no such rule 
drop instance rule nonesuch;

-- no such rule 
drop rewrite rule nonesuch;

