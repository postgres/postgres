CREATE EXTENSION test_dsm_registry;
SELECT set_val_in_shmem(1236);
\c
SELECT get_val_in_shmem();

-- 20 bytes = int (4 bytes) + LWLock (16bytes)
SELECT * FROM pg_dsm_registry;
