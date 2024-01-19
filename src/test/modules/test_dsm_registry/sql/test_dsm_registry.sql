CREATE EXTENSION test_dsm_registry;
SELECT set_val_in_shmem(1236);
\c
SELECT get_val_in_shmem();
