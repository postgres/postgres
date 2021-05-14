create extension spgist_name_ops;

select opcname, amvalidate(opc.oid)
from pg_opclass opc join pg_am am on am.oid = opcmethod
where amname = 'spgist' and opcname = 'name_ops';

-- warning expected here
select opcname, amvalidate(opc.oid)
from pg_opclass opc join pg_am am on am.oid = opcmethod
where amname = 'spgist' and opcname = 'name_ops_old';

create table t(f1 name, f2 integer, f3 text);
create index on t using spgist(f1) include(f2, f3);
\d+ t_f1_f2_f3_idx

insert into t select
  proname,
  case when length(proname) % 2 = 0 then pronargs else null end,
  prosrc from pg_proc;
vacuum analyze t;

explain (costs off)
select * from t
  where f1 > 'binary_upgrade_set_n' and f1 < 'binary_upgrade_set_p'
  order by 1;
select * from t
  where f1 > 'binary_upgrade_set_n' and f1 < 'binary_upgrade_set_p'
  order by 1;

-- Verify clean failure when INCLUDE'd columns result in overlength tuple
-- The error message details are platform-dependent, so show only SQLSTATE
\set VERBOSITY sqlstate
insert into t values(repeat('xyzzy', 12), 42, repeat('xyzzy', 4000));
\set VERBOSITY default

drop index t_f1_f2_f3_idx;

create index on t using spgist(f1 name_ops_old) include(f2, f3);
\d+ t_f1_f2_f3_idx

explain (costs off)
select * from t
  where f1 > 'binary_upgrade_set_n' and f1 < 'binary_upgrade_set_p'
  order by 1;
select * from t
  where f1 > 'binary_upgrade_set_n' and f1 < 'binary_upgrade_set_p'
  order by 1;

\set VERBOSITY sqlstate
insert into t values(repeat('xyzzy', 12), 42, repeat('xyzzy', 4000));
\set VERBOSITY default
