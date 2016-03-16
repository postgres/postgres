CREATE EXTENSION pg_trgm;

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

create index trgm_idx on test_trgm using gist (t gist_trgm_ops);
set enable_seqscan=off;

select t,similarity(t,'qwertyu0988') as sml from test_trgm where t % 'qwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu0988') as sml from test_trgm where t % 'gwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu1988') as sml from test_trgm where t % 'gwertyu1988' order by sml desc, t;
explain (costs off)
select t <-> 'q0987wertyu0988', t from test_trgm order by t <-> 'q0987wertyu0988' limit 2;
select t <-> 'q0987wertyu0988', t from test_trgm order by t <-> 'q0987wertyu0988' limit 2;

drop index trgm_idx;
create index trgm_idx on test_trgm using gin (t gin_trgm_ops);
set enable_seqscan=off;

select t,similarity(t,'qwertyu0988') as sml from test_trgm where t % 'qwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu0988') as sml from test_trgm where t % 'gwertyu0988' order by sml desc, t;
select t,similarity(t,'gwertyu1988') as sml from test_trgm where t % 'gwertyu1988' order by sml desc, t;

create table test2(t text COLLATE "C");
insert into test2 values ('abcdef');
insert into test2 values ('quark');
insert into test2 values ('  z foo bar');
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
