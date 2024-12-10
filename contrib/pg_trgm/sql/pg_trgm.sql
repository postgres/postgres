CREATE EXTENSION pg_trgm;

-- Check whether any of our opclasses fail amvalidate
SELECT amname, opcname
FROM pg_opclass opc LEFT JOIN pg_am am ON am.oid = opcmethod
WHERE opc.oid >= 16384 AND NOT amvalidate(opc.oid);

--backslash is used in tests below, installcheck will fail if
--standard_conforming_string is off
set standard_conforming_strings=on;

-- reduce noise
set extra_float_digits = 0;

select show_trgm('');
select show_trgm('(*&^$@%@');
select show_trgm('a b c');
select show_trgm(' a b c ');
select show_trgm('aA bB cC');
select show_trgm(' aA bB cC ');
select show_trgm('a b C0*%^');

select similarity('wow','WOWa ');
select similarity('wow',' WOW ');

select similarity('---', '####---');

CREATE TABLE test_trgm(t text COLLATE "C");

\copy test_trgm from 'data/trgm.data'

select t,similarity(t,'qwertyu0988') as sml from test_trgm where t % 'qwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu0988') as sml from test_trgm where t % 'gwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu1988') as sml from test_trgm where t % 'gwertyu1988' order by sml desc, t;
select t <-> 'q0987wertyu0988', t from test_trgm order by t <-> 'q0987wertyu0988' limit 2;
select count(*) from test_trgm where t ~ '[qwerty]{2}-?[qwerty]{2}';

create index trgm_idx on test_trgm using gist (t gist_trgm_ops);
set enable_seqscan=off;

select t,similarity(t,'qwertyu0988') as sml from test_trgm where t % 'qwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu0988') as sml from test_trgm where t % 'gwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu1988') as sml from test_trgm where t % 'gwertyu1988' order by sml desc, t;
explain (costs off)
select t <-> 'q0987wertyu0988', t from test_trgm order by t <-> 'q0987wertyu0988' limit 2;
select t <-> 'q0987wertyu0988', t from test_trgm order by t <-> 'q0987wertyu0988' limit 2;
select count(*) from test_trgm where t ~ '[qwerty]{2}-?[qwerty]{2}';

drop index trgm_idx;
create index trgm_idx on test_trgm using gist (t gist_trgm_ops(siglen=0));
create index trgm_idx on test_trgm using gist (t gist_trgm_ops(siglen=2025));
create index trgm_idx on test_trgm using gist (t gist_trgm_ops(siglen=2024));
set enable_seqscan=off;

-- check index compatibility handling when opclass option is specified
alter table test_trgm alter column t type varchar(768);
alter table test_trgm alter column t type text;

select t,similarity(t,'qwertyu0988') as sml from test_trgm where t % 'qwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu0988') as sml from test_trgm where t % 'gwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu1988') as sml from test_trgm where t % 'gwertyu1988' order by sml desc, t;
explain (costs off)
select t <-> 'q0987wertyu0988', t from test_trgm order by t <-> 'q0987wertyu0988' limit 2;
select t <-> 'q0987wertyu0988', t from test_trgm order by t <-> 'q0987wertyu0988' limit 2;
select count(*) from test_trgm where t ~ '[qwerty]{2}-?[qwerty]{2}';

drop index trgm_idx;
create index trgm_idx on test_trgm using gin (t gin_trgm_ops);
set enable_seqscan=off;

select t,similarity(t,'qwertyu0988') as sml from test_trgm where t % 'qwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu0988') as sml from test_trgm where t % 'gwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu1988') as sml from test_trgm where t % 'gwertyu1988' order by sml desc, t;
select count(*) from test_trgm where t ~ '[qwerty]{2}-?[qwerty]{2}';

-- check handling of indexquals that generate no searchable conditions
explain (costs off)
select count(*) from test_trgm where t like '%99%' and t like '%qwerty%';
select count(*) from test_trgm where t like '%99%' and t like '%qwerty%';
explain (costs off)
select count(*) from test_trgm where t like '%99%' and t like '%qw%';
select count(*) from test_trgm where t like '%99%' and t like '%qw%';
-- ensure that pending-list items are handled correctly, too
create temp table t_test_trgm(t text COLLATE "C");
create index t_trgm_idx on t_test_trgm using gin (t gin_trgm_ops);
insert into t_test_trgm values ('qwerty99'), ('qwerty01');
explain (costs off)
select count(*) from t_test_trgm where t like '%99%' and t like '%qwerty%';
select count(*) from t_test_trgm where t like '%99%' and t like '%qwerty%';
explain (costs off)
select count(*) from t_test_trgm where t like '%99%' and t like '%qw%';
select count(*) from t_test_trgm where t like '%99%' and t like '%qw%';

-- run the same queries with sequential scan to check the results
set enable_bitmapscan=off;
set enable_seqscan=on;
select count(*) from test_trgm where t like '%99%' and t like '%qwerty%';
select count(*) from test_trgm where t like '%99%' and t like '%qw%';
select count(*) from t_test_trgm where t like '%99%' and t like '%qwerty%';
select count(*) from t_test_trgm where t like '%99%' and t like '%qw%';
reset enable_bitmapscan;

create table test2(t text COLLATE "C");
insert into test2 values ('abcdef');
insert into test2 values ('quark');
insert into test2 values ('  z foo bar');
insert into test2 values ('/123/-45/');
insert into test2 values ('line 1');
insert into test2 values ('%line 2');
insert into test2 values ('line 3%');
insert into test2 values ('%line 4%');
insert into test2 values ('%li%ne 5%');
insert into test2 values ('li_e 6');
create index test2_idx_gin on test2 using gin (t gin_trgm_ops);
set enable_seqscan=off;
explain (costs off)
  select * from test2 where t like '%BCD%';
explain (costs off)
  select * from test2 where t ilike '%BCD%';
select * from test2 where t like '%BCD%';
select * from test2 where t like '%bcd%';
select * from test2 where t like E'%\\bcd%';
select * from test2 where t ilike '%BCD%';
select * from test2 where t ilike 'qua%';
select * from test2 where t like '%z foo bar%';
select * from test2 where t like '  z foo%';
explain (costs off)
  select * from test2 where t ~ '[abc]{3}';
explain (costs off)
  select * from test2 where t ~* 'DEF';
select * from test2 where t ~ '[abc]{3}';
select * from test2 where t ~ 'a[bc]+d';
select * from test2 where t ~ '(abc)*$';
select * from test2 where t ~* 'DEF';
select * from test2 where t ~ 'dEf';
select * from test2 where t ~* '^q';
select * from test2 where t ~* '[abc]{3}[def]{3}';
select * from test2 where t ~* 'ab[a-z]{3}';
select * from test2 where t ~* '(^| )qua';
select * from test2 where t ~ 'q.*rk$';
select * from test2 where t ~ 'q';
select * from test2 where t ~ '[a-z]{3}';
select * from test2 where t ~* '(a{10}|b{10}|c{10}){10}';
select * from test2 where t ~ 'z foo bar';
select * from test2 where t ~ ' z foo bar';
select * from test2 where t ~ '  z foo bar';
select * from test2 where t ~ '  z foo';
select * from test2 where t ~ 'qua(?!foo)';
select * from test2 where t ~ '/\d+/-\d';
-- test = operator
explain (costs off)
  select * from test2 where t = 'abcdef';
select * from test2 where t = 'abcdef';
explain (costs off)
  select * from test2 where t = '%line%';
select * from test2 where t = '%line%';
select * from test2 where t = 'li_e 1';
select * from test2 where t = '%line 2';
select * from test2 where t = 'line 3%';
select * from test2 where t = '%line 3%';
select * from test2 where t = '%line 4%';
select * from test2 where t = '%line 5%';
select * from test2 where t = '%li_ne 5%';
select * from test2 where t = '%li%ne 5%';
select * from test2 where t = 'line 6';
select * from test2 where t = 'li_e 6';
drop index test2_idx_gin;

create index test2_idx_gist on test2 using gist (t gist_trgm_ops);
set enable_seqscan=off;
explain (costs off)
  select * from test2 where t like '%BCD%';
explain (costs off)
  select * from test2 where t ilike '%BCD%';
select * from test2 where t like '%BCD%';
select * from test2 where t like '%bcd%';
select * from test2 where t like E'%\\bcd%';
select * from test2 where t ilike '%BCD%';
select * from test2 where t ilike 'qua%';
select * from test2 where t like '%z foo bar%';
select * from test2 where t like '  z foo%';
explain (costs off)
  select * from test2 where t ~ '[abc]{3}';
explain (costs off)
  select * from test2 where t ~* 'DEF';
select * from test2 where t ~ '[abc]{3}';
select * from test2 where t ~ 'a[bc]+d';
select * from test2 where t ~ '(abc)*$';
select * from test2 where t ~* 'DEF';
select * from test2 where t ~ 'dEf';
select * from test2 where t ~* '^q';
select * from test2 where t ~* '[abc]{3}[def]{3}';
select * from test2 where t ~* 'ab[a-z]{3}';
select * from test2 where t ~* '(^| )qua';
select * from test2 where t ~ 'q.*rk$';
select * from test2 where t ~ 'q';
select * from test2 where t ~ '[a-z]{3}';
select * from test2 where t ~* '(a{10}|b{10}|c{10}){10}';
select * from test2 where t ~ 'z foo bar';
select * from test2 where t ~ ' z foo bar';
select * from test2 where t ~ '  z foo bar';
select * from test2 where t ~ '  z foo';
select * from test2 where t ~ 'qua(?!foo)';
select * from test2 where t ~ '/\d+/-\d';
-- test = operator
explain (costs off)
  select * from test2 where t = 'abcdef';
select * from test2 where t = 'abcdef';
explain (costs off)
  select * from test2 where t = '%line%';
select * from test2 where t = '%line%';
select * from test2 where t = 'li_e 1';
select * from test2 where t = '%line 2';
select * from test2 where t = 'line 3%';
select * from test2 where t = '%line 3%';
select * from test2 where t = '%line 4%';
select * from test2 where t = '%line 5%';
select * from test2 where t = '%li_ne 5%';
select * from test2 where t = '%li%ne 5%';
select * from test2 where t = 'line 6';
select * from test2 where t = 'li_e 6';

-- Check similarity threshold (bug #14202)

CREATE TEMP TABLE restaurants (city text);
INSERT INTO restaurants SELECT 'Warsaw' FROM generate_series(1, 10000);
INSERT INTO restaurants SELECT 'Szczecin' FROM generate_series(1, 10000);
CREATE INDEX ON restaurants USING gist(city gist_trgm_ops);

-- Similarity of the two names (for reference).
SELECT similarity('Szczecin', 'Warsaw');

-- Should get only 'Warsaw' for either setting of set_limit.
EXPLAIN (COSTS OFF)
SELECT DISTINCT city, similarity(city, 'Warsaw'), show_limit()
  FROM restaurants WHERE city % 'Warsaw';
SELECT set_limit(0.3);
SELECT DISTINCT city, similarity(city, 'Warsaw'), show_limit()
  FROM restaurants WHERE city % 'Warsaw';
SELECT set_limit(0.5);
SELECT DISTINCT city, similarity(city, 'Warsaw'), show_limit()
  FROM restaurants WHERE city % 'Warsaw';
