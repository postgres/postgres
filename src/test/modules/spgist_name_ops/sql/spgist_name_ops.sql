create extension spgist_name_ops;

select opcname, amvalidate(opc.oid)
from pg_opclass opc join pg_am am on am.oid = opcmethod
where amname = 'spgist' and opcname = 'name_ops';

-- warning expected here
select opcname, amvalidate(opc.oid)
from pg_opclass opc join pg_am am on am.oid = opcmethod
where amname = 'spgist' and opcname = 'name_ops_old';

create table t(f1 name);
create index on t using spgist(f1);
\d+ t_f1_idx

insert into t select proname from pg_proc;
vacuum analyze t;

explain (costs off)
select * from t
  where f1 > 'binary_upgrade_set_n' and f1 < 'binary_upgrade_set_p'
  order by 1;
select * from t
  where f1 > 'binary_upgrade_set_n' and f1 < 'binary_upgrade_set_p'
  order by 1;

drop index t_f1_idx;

create index on t using spgist(f1 name_ops_old);
\d+ t_f1_idx

explain (costs off)
select * from t
  where f1 > 'binary_upgrade_set_n' and f1 < 'binary_upgrade_set_p'
  order by 1;
select * from t
  where f1 > 'binary_upgrade_set_n' and f1 < 'binary_upgrade_set_p'
  order by 1;
