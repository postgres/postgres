CREATE EXTENSION test_dsm_registry;
SELECT set_val_in_dsm(1236);
SELECT set_val_in_hash('test', '1414');
\c
SELECT get_val_in_dsm();
SELECT get_val_in_hash('test');
