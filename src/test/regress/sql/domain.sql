

-- Test Comment / Drop
create domain domaindroptest int4;
comment on domain domaindroptest is 'About to drop this..';

-- currently this will be disallowed
create domain basetypetest domaindroptest;

drop domain domaindroptest;

-- this should fail because already gone
drop domain domaindroptest cascade;


-- TEST Domains.

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

drop table basictest;
drop domain domainvarchar restrict;
drop domain domainnumeric restrict;
drop domain domainint4 restrict;
drop domain domaintext;


-- Array Test
create domain domainint4arr int4[1];
create domain domaintextarr text[2][3];

create table domarrtest
           ( testint4arr domainint4arr
           , testtextarr domaintextarr
            );
INSERT INTO domarrtest values ('{2,2}', '{{"a","b"}{"c","d"}}');
INSERT INTO domarrtest values ('{{2,2}{2,2}}', '{{"a","b"}}');
INSERT INTO domarrtest values ('{2,2}', '{{"a","b"}{"c","d"}{"e"}}');
INSERT INTO domarrtest values ('{2,2}', '{{"a"}{"c"}}');
INSERT INTO domarrtest values (NULL, '{{"a","b"}{"c","d","e"}}');
select * from domarrtest;
select testint4arr[1], testtextarr[2:2] from domarrtest;

drop table domarrtest;
drop domain domainint4arr restrict;
drop domain domaintextarr restrict;


create domain dnotnull varchar(15) NOT NULL;
create domain dnull    varchar(15);

create table nulltest
           ( col1 dnotnull
           , col2 dnotnull NULL  -- NOT NULL in the domain cannot be overridden
           , col3 dnull    NOT NULL
           , col4 dnull
           );
INSERT INTO nulltest DEFAULT VALUES;
INSERT INTO nulltest values ('a', 'b', 'c', 'd');  -- Good
INSERT INTO nulltest values (NULL, 'b', 'c', 'd');
INSERT INTO nulltest values ('a', NULL, 'c', 'd');
INSERT INTO nulltest values ('a', 'b', NULL, 'd');
INSERT INTO nulltest values ('a', 'b', 'c', NULL); -- Good

-- Test copy
COPY nulltest FROM stdin; --fail
a	b	\N	d
\.

COPY nulltest FROM stdin;
a	b	c	\N
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


create domain ddef1 int4 DEFAULT 3;
create domain ddef2 oid DEFAULT '12';
-- Type mixing, function returns int8
create domain ddef3 text DEFAULT 5;
create sequence ddef4_seq;
create domain ddef4 int4 DEFAULT nextval(cast('ddef4_seq' as text));
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
insert into defaulttest default values;
insert into defaulttest default values;
insert into defaulttest default values;

-- Test defaults with copy
COPY defaulttest(col5) FROM stdin;
42
\.

select * from defaulttest;

drop sequence ddef4_seq;
drop table defaulttest;
drop domain ddef1 restrict;
drop domain ddef2 restrict;
drop domain ddef3 restrict;
drop domain ddef4 restrict;
drop domain ddef5 restrict;
