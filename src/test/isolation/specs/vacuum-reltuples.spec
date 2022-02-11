# Test for vacuum's handling of reltuples when pages are skipped due
# to page pins. We absolutely need to avoid setting reltuples=0 in
# such cases, since that interferes badly with planning.
#
# Expected result for all three permutation is 21 tuples, including
# the second permutation.  VACUUM is able to count the concurrently
# inserted tuple in its final reltuples, even when a cleanup lock
# cannot be acquired on the affected heap page.

setup {
    create table smalltbl
        as select i as id from generate_series(1,20) i;
    alter table smalltbl set (autovacuum_enabled = off);
}
setup {
    vacuum analyze smalltbl;
}

teardown {
    drop table smalltbl;
}

session worker
step open {
    begin;
    declare c1 cursor for select 1 as dummy from smalltbl;
}
step fetch1 {
    fetch next from c1;
}
step close {
    commit;
}
step stats {
    select relpages, reltuples from pg_class
     where oid='smalltbl'::regclass;
}

session vacuumer
step vac {
    vacuum smalltbl;
}
step modify {
    insert into smalltbl select max(id)+1 from smalltbl;
}

permutation modify vac stats
permutation modify open fetch1 vac close stats
permutation modify vac stats
