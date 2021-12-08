# REINDEX CONCURRENTLY with toast relations
#
# Ensure that concurrent operations work correctly when a REINDEX is performed
# concurrently on toast relations.  Toast relation names are not deterministic,
# so this abuses of allow_system_table_mods to change the names of toast
# tables and its indexes so as they can be executed with REINDEX CONCURRENTLY,
# which cannot be launched in a transaction context.

# Create a table, with deterministic names for its toast relation and indexes.
# Fortunately ALTER TABLE is transactional, making the renaming of toast
# relations possible with allow_system_table_mods.
setup
{
    CREATE TABLE reind_con_wide(id int primary key, data text);
    INSERT INTO reind_con_wide
      SELECT 1, repeat('1', 11) || string_agg(g.i::text || random()::text, '') FROM generate_series(1, 500) g(i);
    INSERT INTO reind_con_wide
      SELECT 2, repeat('2', 11) || string_agg(g.i::text || random()::text, '') FROM generate_series(1, 500) g(i);
    SET allow_system_table_mods TO true;
    DO $$DECLARE r record;
      BEGIN
      SELECT INTO r reltoastrelid::regclass::text AS table_name FROM pg_class
        WHERE oid = 'reind_con_wide'::regclass;
      EXECUTE 'ALTER TABLE ' || r.table_name || ' RENAME TO reind_con_toast;';
      SELECT INTO r indexrelid::regclass::text AS index_name FROM pg_index
        WHERE indrelid = (SELECT oid FROM pg_class where relname = 'reind_con_toast');
      EXECUTE 'ALTER INDEX ' || r.index_name || ' RENAME TO reind_con_toast_idx;';
    END$$;
}

teardown
{
    DROP TABLE IF EXISTS reind_con_wide;
}

session s1
setup { BEGIN; }
step lrex1 { lock TABLE reind_con_wide in ROW EXCLUSIVE MODE; }
step lsha1 { lock TABLE reind_con_wide in SHARE MODE; }
step lexc1 { lock TABLE reind_con_wide in EXCLUSIVE MODE; }
step ins1 { INSERT INTO reind_con_wide SELECT 3, repeat('3', 11) || string_agg(g.i::text || random()::text, '') FROM generate_series(1, 500) g(i); }
step upd1 { UPDATE reind_con_wide SET data = (SELECT repeat('4', 11) || string_agg(g.i::text || random()::text, '') FROM generate_series(1, 500) g(i)) WHERE id = 1; }
step del1 { DELETE FROM reind_con_wide WHERE id = 2; }
step dro1 { DROP TABLE reind_con_wide; }
step end1 { COMMIT; }
step rol1 { ROLLBACK; }

session s2
step retab2 { REINDEX TABLE CONCURRENTLY pg_toast.reind_con_toast; }
step reind2 { REINDEX INDEX CONCURRENTLY pg_toast.reind_con_toast_idx; }
step sel2 { SELECT id, substr(data, 1, 10) FROM reind_con_wide ORDER BY id; }

# Transaction commit with ROW EXCLUSIVE MODE
permutation lrex1 ins1 retab2 end1 sel2
permutation lrex1 ins1 reind2 end1 sel2
permutation lrex1 upd1 retab2 end1 sel2
permutation lrex1 upd1 reind2 end1 sel2
permutation lrex1 del1 retab2 end1 sel2
permutation lrex1 del1 reind2 end1 sel2
permutation lrex1 dro1 retab2 end1 sel2
permutation lrex1 dro1 reind2 end1 sel2
permutation lrex1 retab2 dro1 end1 sel2
permutation lrex1 reind2 dro1 end1 sel2
# Transaction commit with SHARE MODE
permutation lsha1 ins1 retab2 end1 sel2
permutation lsha1 ins1 reind2 end1 sel2
permutation lsha1 upd1 retab2 end1 sel2
permutation lsha1 upd1 reind2 end1 sel2
permutation lsha1 del1 retab2 end1 sel2
permutation lsha1 del1 reind2 end1 sel2
permutation lsha1 dro1 retab2 end1 sel2
permutation lsha1 dro1 reind2 end1 sel2
permutation lsha1 retab2 dro1 end1 sel2
permutation lsha1 reind2 dro1 end1 sel2
# Transaction commit with EXCLUSIVE MODE
permutation lexc1 ins1 retab2 end1 sel2
permutation lexc1 ins1 reind2 end1 sel2
permutation lexc1 upd1 retab2 end1 sel2
permutation lexc1 upd1 reind2 end1 sel2
permutation lexc1 del1 retab2 end1 sel2
permutation lexc1 del1 reind2 end1 sel2
permutation lexc1 dro1 retab2 end1 sel2
permutation lexc1 dro1 reind2 end1 sel2
permutation lexc1 retab2 dro1 end1 sel2
permutation lexc1 reind2 dro1 end1 sel2

# Transaction rollback with ROW EXCLUSIVE MODE
permutation lrex1 ins1 retab2 rol1 sel2
permutation lrex1 ins1 reind2 rol1 sel2
permutation lrex1 upd1 retab2 rol1 sel2
permutation lrex1 upd1 reind2 rol1 sel2
permutation lrex1 del1 retab2 rol1 sel2
permutation lrex1 del1 reind2 rol1 sel2
permutation lrex1 dro1 retab2 rol1 sel2
permutation lrex1 dro1 reind2 rol1 sel2
permutation lrex1 retab2 dro1 rol1 sel2
permutation lrex1 reind2 dro1 rol1 sel2
# Transaction rollback with SHARE MODE
permutation lsha1 ins1 retab2 rol1 sel2
permutation lsha1 ins1 reind2 rol1 sel2
permutation lsha1 upd1 retab2 rol1 sel2
permutation lsha1 upd1 reind2 rol1 sel2
permutation lsha1 del1 retab2 rol1 sel2
permutation lsha1 del1 reind2 rol1 sel2
permutation lsha1 dro1 retab2 rol1 sel2
permutation lsha1 dro1 reind2 rol1 sel2
permutation lsha1 retab2 dro1 rol1 sel2
permutation lsha1 reind2 dro1 rol1 sel2
# Transaction rollback with EXCLUSIVE MODE
permutation lexc1 ins1 retab2 rol1 sel2
permutation lexc1 ins1 reind2 rol1 sel2
permutation lexc1 upd1 retab2 rol1 sel2
permutation lexc1 upd1 reind2 rol1 sel2
permutation lexc1 del1 retab2 rol1 sel2
permutation lexc1 del1 reind2 rol1 sel2
permutation lexc1 dro1 retab2 rol1 sel2
permutation lexc1 dro1 reind2 rol1 sel2
permutation lexc1 retab2 dro1 rol1 sel2
permutation lexc1 reind2 dro1 rol1 sel2
