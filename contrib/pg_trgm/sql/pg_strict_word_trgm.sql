DROP INDEX trgm_idx2;

\copy test_trgm3 from 'data/trgm2.data'

-- reduce noise
set extra_float_digits = 0;

select t,strict_word_similarity('Baykal',t) as sml from test_trgm2 where 'Baykal' <<% t order by sml desc, t;
select t,strict_word_similarity('Kabankala',t) as sml from test_trgm2 where 'Kabankala' <<% t order by sml desc, t;
select t,strict_word_similarity('Baykal',t) as sml from test_trgm2 where t %>> 'Baykal' order by sml desc, t;
select t,strict_word_similarity('Kabankala',t) as sml from test_trgm2 where t %>> 'Kabankala' order by sml desc, t;
select t <->>> 'Alaikallupoddakulam', t from test_trgm2 order by t <->>> 'Alaikallupoddakulam' limit 7;

create index trgm_idx2 on test_trgm2 using gist (t gist_trgm_ops);
set enable_seqscan=off;

select t,strict_word_similarity('Baykal',t) as sml from test_trgm2 where 'Baykal' <<% t order by sml desc, t;
select t,strict_word_similarity('Kabankala',t) as sml from test_trgm2 where 'Kabankala' <<% t order by sml desc, t;
select t,strict_word_similarity('Baykal',t) as sml from test_trgm2 where t %>> 'Baykal' order by sml desc, t;
select t,strict_word_similarity('Kabankala',t) as sml from test_trgm2 where t %>> 'Kabankala' order by sml desc, t;

explain (costs off)
select t <->>> 'Alaikallupoddakulam', t from test_trgm2 order by t <->>> 'Alaikallupoddakulam' limit 7;
select t <->>> 'Alaikallupoddakulam', t from test_trgm2 order by t <->>> 'Alaikallupoddakulam' limit 7;

drop index trgm_idx2;
create index trgm_idx2 on test_trgm2 using gin (t gin_trgm_ops);
set enable_seqscan=off;

select t,strict_word_similarity('Baykal',t) as sml from test_trgm2 where 'Baykal' <<% t order by sml desc, t;
select t,strict_word_similarity('Kabankala',t) as sml from test_trgm2 where 'Kabankala' <<% t order by sml desc, t;
select t,strict_word_similarity('Baykal',t) as sml from test_trgm2 where t %>> 'Baykal' order by sml desc, t;
select t,strict_word_similarity('Kabankala',t) as sml from test_trgm2 where t %>> 'Kabankala' order by sml desc, t;

set "pg_trgm.strict_word_similarity_threshold" to 0.4;
select t,strict_word_similarity('Baykal',t) as sml from test_trgm2 where 'Baykal' <<% t order by sml desc, t;
select t,strict_word_similarity('Kabankala',t) as sml from test_trgm2 where 'Kabankala' <<% t order by sml desc, t;
select t,strict_word_similarity('Baykal',t) as sml from test_trgm2 where t %>> 'Baykal' order by sml desc, t;
select t,strict_word_similarity('Kabankala',t) as sml from test_trgm2 where t %>> 'Kabankala' order by sml desc, t;

set "pg_trgm.strict_word_similarity_threshold" to 0.2;
select t,strict_word_similarity('Baykal',t) as sml from test_trgm2 where 'Baykal' <<% t order by sml desc, t;
select t,strict_word_similarity('Kabankala',t) as sml from test_trgm2 where 'Kabankala' <<% t order by sml desc, t;
select t,strict_word_similarity('Baykal',t) as sml from test_trgm2 where t %>> 'Baykal' order by sml desc, t;
select t,strict_word_similarity('Kabankala',t) as sml from test_trgm2 where t %>> 'Kabankala' order by sml desc, t;
