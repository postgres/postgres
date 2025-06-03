CREATE EXTENSION test_dsm_registry;
SELECT set_val_in_shmem(1236);
\c
SELECT get_val_in_shmem();

SELECT size > 0 FROM pg_dsm_registry_allocations WHERE name = 'test_dsm_registry';
