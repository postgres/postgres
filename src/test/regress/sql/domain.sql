--
-- Test domains.
--

-- Test Comment / Drop
create domain domaindroptest int4;
comment on domain domaindroptest is 'About to drop this..';

create domain dependenttypetest domaindroptest;

-- fail because of dependent type
drop domain domaindroptest;

drop domain domaindroptest cascade;

-- this should fail because already gone
drop domain domaindroptest cascade;


-- Test domain input.

-- Note: the point of checking both INSERT and COPY FROM is that INSERT
-- exercises CoerceToDomain while COPY exercises domain_in.

create domain domainvarchar varchar(5);
create domain domainnumeric numeric(8,2);
create domain domainint4 int4;
create domain domaintext text;

-- Test explicit coercions --- these should succeed (and truncate)
SELECT cast('123456' as domainvarchar);
SELECT cast('12345' as domainvarchar);

-- Test tables using domains
create table basictest
           ( testint4 domainint4
           , testtext domaintext
           , testvarchar domainvarchar
           , testnumeric domainnumeric
           );

INSERT INTO basictest values ('88', 'haha', 'short', '123.12');      -- Good
INSERT INTO basictest values ('88', 'haha', 'short text', '123.12'); -- Bad varchar
INSERT INTO basictest values ('88', 'haha', 'short', '123.1212');    -- Truncate numeric

-- Test copy
COPY basictest (testvarchar) FROM stdin; -- fail
notsoshorttext
\.

COPY basictest (testvarchar) FROM stdin;
short
\.

select * from basictest;

-- check that domains inherit operations from base types
select testtext || testvarchar as concat, testnumeric + 42 as sum
from basictest;

-- check that union/case/coalesce type resolution handles domains properly
select coalesce(4::domainint4, 7) is of (int4) as t;
select coalesce(4::domainint4, 7) is of (domainint4) as f;
select coalesce(4::domainint4, 7::domainint4) is of (domainint4) as t;

drop table basictest;
drop domain domainvarchar restrict;
drop domain domainnumeric restrict;
drop domain domainint4 restrict;
drop domain domaintext;


-- Test domains over array types

create domain domainint4arr int4[1];
create domain domainchar4arr varchar(4)[2][3];

create table domarrtest
           ( testint4arr domainint4arr
           , testchar4arr domainchar4arr
            );
INSERT INTO domarrtest values ('{2,2}', '{{"a","b"},{"c","d"}}');
INSERT INTO domarrtest values ('{{2,2},{2,2}}', '{{"a","b"}}');
INSERT INTO domarrtest values ('{2,2}', '{{"a","b"},{"c","d"},{"e","f"}}');
INSERT INTO domarrtest values ('{2,2}', '{{"a"},{"c"}}');
INSERT INTO domarrtest values (NULL, '{{"a","b","c"},{"d","e","f"}}');
INSERT INTO domarrtest values (NULL, '{{"toolong","b","c"},{"d","e","f"}}');
select * from domarrtest;
select testint4arr[1], testchar4arr[2:2] from domarrtest;

COPY domarrtest FROM stdin;
{3,4}	{q,w,e}
\N	\N
\.

COPY domarrtest FROM stdin;	-- fail
{3,4}	{qwerty,w,e}
\.

select * from domarrtest;

drop table domarrtest;
drop domain domainint4arr restrict;
drop domain domainchar4arr restrict;


create domain dnotnull varchar(15) NOT NULL;
create domain dnull    varchar(15);
create domain dcheck   varchar(15) NOT NULL CHECK (VALUE = 'a' OR VALUE = 'c' OR VALUE = 'd');

create table nulltest
           ( col1 dnotnull
           , col2 dnotnull NULL  -- NOT NULL in the domain cannot be overridden
           , col3 dnull    NOT NULL
           , col4 dnull
           , col5 dcheck CHECK (col5 IN ('c', 'd'))
           );
INSERT INTO nulltest DEFAULT VALUES;
INSERT INTO nulltest values ('a', 'b', 'c', 'd', 'c');  -- Good
insert into nulltest values ('a', 'b', 'c', 'd', NULL);
insert into nulltest values ('a', 'b', 'c', 'd', 'a');
INSERT INTO nulltest values (NULL, 'b', 'c', 'd', 'd');
INSERT INTO nulltest values ('a', NULL, 'c', 'd', 'c');
INSERT INTO nulltest values ('a', 'b', NULL, 'd', 'c');
INSERT INTO nulltest values ('a', 'b', 'c', NULL, 'd'); -- Good

-- Test copy
COPY nulltest FROM stdin; --fail
a	b	\N	d	d
\.

COPY nulltest FROM stdin; --fail
a	b	c	d	\N
\.

-- Last row is bad
COPY nulltest FROM stdin;
a	b	c	\N	c
a	b	c	\N	d
a	b	c	\N	a
\.

select * from nulltest;

-- Test out coerced (casted) constraints
SELECT cast('1' as dnotnull);
SELECT cast(NULL as dnotnull); -- fail
SELECT cast(cast(NULL as dnull) as dnotnull); -- fail
SELECT cast(col4 as dnotnull) from nulltest; -- fail

-- cleanup
drop table nulltest;
drop domain dnotnull restrict;
drop domain dnull restrict;
drop domain dcheck restrict;


create domain ddef1 int4 DEFAULT 3;
create domain ddef2 oid DEFAULT '12';
-- Type mixing, function returns int8
create domain ddef3 text DEFAULT 5;
create sequence ddef4_seq;
create domain ddef4 int4 DEFAULT nextval('ddef4_seq');
create domain ddef5 numeric(8,2) NOT NULL DEFAULT '12.12';

create table defaulttest
            ( col1 ddef1
            , col2 ddef2
            , col3 ddef3
            , col4 ddef4 PRIMARY KEY
            , col5 ddef1 NOT NULL DEFAULT NULL
            , col6 ddef2 DEFAULT '88'
            , col7 ddef4 DEFAULT 8000
            , col8 ddef5
            );
insert into defaulttest(col4) values(0); -- fails, col5 defaults to null
alter table defaulttest alter column col5 drop default;
insert into defaulttest default values; -- succeeds, inserts domain default
-- We used to treat SET DEFAULT NULL as equivalent to DROP DEFAULT; wrong
alter table defaulttest alter column col5 set default null;
insert into defaulttest(col4) values(0); -- fails
alter table defaulttest alter column col5 drop default;
insert into defaulttest default values;
insert into defaulttest default values;

-- Test defaults with copy
COPY defaulttest(col5) FROM stdin;
42
\.

select * from defaulttest;

drop table defaulttest cascade;

-- Test ALTER DOMAIN .. NOT NULL
create domain dnotnulltest integer;
create table domnotnull
( col1 dnotnulltest
, col2 dnotnulltest
);

insert into domnotnull default values;
alter domain dnotnulltest set not null; -- fails

update domnotnull set col1 = 5;
alter domain dnotnulltest set not null; -- fails

update domnotnull set col2 = 6;

alter domain dnotnulltest set not null;

update domnotnull set col1 = null; -- fails

alter domain dnotnulltest drop not null;

update domnotnull set col1 = null;

drop domain dnotnulltest cascade;

-- Test ALTER DOMAIN .. DEFAULT ..
create table domdeftest (col1 ddef1);

insert into domdeftest default values;
select * from domdeftest;

alter domain ddef1 set default '42';
insert into domdeftest default values;
select * from domdeftest;

alter domain ddef1 drop default;
insert into domdeftest default values;
select * from domdeftest;

drop table domdeftest;

-- Test ALTER DOMAIN .. CONSTRAINT ..
create domain con as integer;
create table domcontest (col1 con);

insert into domcontest values (1);
insert into domcontest values (2);
alter domain con add constraint t check (VALUE < 1); -- fails

alter domain con add constraint t check (VALUE < 34);
alter domain con add check (VALUE > 0);

insert into domcontest values (-5); -- fails
insert into domcontest values (42); -- fails
insert into domcontest values (5);

alter domain con drop constraint t;
insert into domcontest values (-5); --fails
insert into domcontest values (42);

-- Confirm ALTER DOMAIN with RULES.
create table domtab (col1 integer);
create domain dom as integer;
create view domview as select cast(col1 as dom) from domtab;
insert into domtab (col1) values (null);
insert into domtab (col1) values (5);
select * from domview;

alter domain dom set not null;
select * from domview; -- fail

alter domain dom drop not null;
select * from domview;

alter domain dom add constraint domchkgt6 check(value > 6);
select * from domview; --fail

alter domain dom drop constraint domchkgt6 restrict;
select * from domview;

-- cleanup
drop domain ddef1 restrict;
drop domain ddef2 restrict;
drop domain ddef3 restrict;
drop domain ddef4 restrict;
drop domain ddef5 restrict;
drop sequence ddef4_seq;

-- Test domains over domains
create domain vchar4 varchar(4);
create domain dinter vchar4 check (substring(VALUE, 1, 1) = 'x');
create domain dtop dinter check (substring(VALUE, 2, 1) = '1');

select 'x123'::dtop;
select 'x1234'::dtop; -- explicit coercion should truncate
select 'y1234'::dtop; -- fail
select 'y123'::dtop; -- fail
select 'yz23'::dtop; -- fail
select 'xz23'::dtop; -- fail

create temp table dtest(f1 dtop);

insert into dtest values('x123');
insert into dtest values('x1234'); -- fail, implicit coercion
insert into dtest values('y1234'); -- fail, implicit coercion
insert into dtest values('y123'); -- fail
insert into dtest values('yz23'); -- fail
insert into dtest values('xz23'); -- fail

drop table dtest;
drop domain vchar4 cascade;

-- Make sure that constraints of newly-added domain columns are
-- enforced correctly, even if there's no default value for the new
-- column. Per bug #1433
create domain str_domain as text not null;

create table domain_test (a int, b int);

insert into domain_test values (1, 2);
insert into domain_test values (1, 2);

-- should fail
alter table domain_test add column c str_domain;

create domain str_domain2 as text check (value <> 'foo') default 'foo';

-- should fail
alter table domain_test add column d str_domain2;

-- Check that domain constraints on prepared statement parameters of
-- unknown type are enforced correctly.
create domain pos_int as int4 check (value > 0) not null;
prepare s1 as select $1::pos_int = 10 as "is_ten";

execute s1(10);
execute s1(0); -- should fail
execute s1(NULL); -- should fail

-- Check that domain constraints on plpgsql function parameters, results,
-- and local variables are enforced correctly.

create function doubledecrement(p1 pos_int) returns pos_int as $$
declare v pos_int;
begin
    return p1;
end$$ language plpgsql;

select doubledecrement(3); -- fail because of implicit null assignment

create or replace function doubledecrement(p1 pos_int) returns pos_int as $$
declare v pos_int := 0;
begin
    return p1;
end$$ language plpgsql;

select doubledecrement(3); -- fail at initialization assignment

create or replace function doubledecrement(p1 pos_int) returns pos_int as $$
declare v pos_int := 1;
begin
    v := p1 - 1;
    return v - 1;
end$$ language plpgsql;

select doubledecrement(null); -- fail before call
select doubledecrement(0); -- fail before call
select doubledecrement(1); -- fail at assignment to v
select doubledecrement(2); -- fail at return
select doubledecrement(3); -- good

-- Check that ALTER DOMAIN tests columns of derived types

create domain posint as int4;

-- Currently, this doesn't work for composite types, but verify it complains
create type ddtest1 as (f1 posint);
create table ddtest2(f1 ddtest1);
insert into ddtest2 values(row(-1));
alter domain posint add constraint c1 check(value >= 0);
drop table ddtest2;

create table ddtest2(f1 ddtest1[]);
insert into ddtest2 values('{(-1)}');
alter domain posint add constraint c1 check(value >= 0);
drop table ddtest2;

alter domain posint add constraint c1 check(value >= 0);

create domain posint2 as posint check (value % 2 = 0);
create table ddtest2(f1 posint2);
insert into ddtest2 values(11); -- fail
insert into ddtest2 values(-2); -- fail
insert into ddtest2 values(2);

alter domain posint add constraint c2 check(value >= 10); -- fail
alter domain posint add constraint c2 check(value > 0); -- OK

drop table ddtest2;
drop type ddtest1;
drop domain posint cascade;
