SELECT name, type, size IS DISTINCT FROM 0 AS size
FROM pg_dsm_registry_allocations
WHERE name like 'test_dsm_registry%' ORDER BY name;
CREATE EXTENSION test_dsm_registry;
SELECT set_val_in_shmem(1236);
SELECT set_val_in_hash('test', '1414');
\c
SELECT get_val_in_shmem();
SELECT get_val_in_hash('test');
\c
SELECT name, type, size IS DISTINCT FROM 0 AS size
FROM pg_dsm_registry_allocations
WHERE name like 'test_dsm_registry%' ORDER BY name;
