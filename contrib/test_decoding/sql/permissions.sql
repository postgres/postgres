-- predictability
SET synchronous_commit = on;

-- setup
CREATE ROLE lr_normal;
CREATE ROLE lr_superuser SUPERUSER;
CREATE ROLE lr_replication REPLICATION;
CREATE TABLE lr_test(data text);

-- superuser can control replication
SET ROLE lr_superuser;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
INSERT INTO lr_test VALUES('lr_superuser_init');
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT pg_drop_replication_slot('regression_slot');
RESET ROLE;

-- replication user can control replication
SET ROLE lr_replication;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
INSERT INTO lr_test VALUES('lr_superuser_init');
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT pg_drop_replication_slot('regression_slot');
RESET ROLE;

-- plain user *can't* can control replication
SET ROLE lr_normal;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
INSERT INTO lr_test VALUES('lr_superuser_init');
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT pg_drop_replication_slot('regression_slot');
RESET ROLE;

-- replication users can drop superuser created slots
SET ROLE lr_superuser;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
RESET ROLE;
SET ROLE lr_replication;
SELECT pg_drop_replication_slot('regression_slot');
RESET ROLE;

-- normal users can't drop existing slots
SET ROLE lr_superuser;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
RESET ROLE;
SET ROLE lr_normal;
SELECT pg_drop_replication_slot('regression_slot');
RESET ROLE;

-- all users can see existing slots
SET ROLE lr_superuser;
SELECT slot_name, plugin FROM pg_replication_slots;
RESET ROLE;

SET ROLE lr_replication;
SELECT slot_name, plugin FROM pg_replication_slots;
RESET ROLE;

SET ROLE lr_normal;
SELECT slot_name, plugin FROM pg_replication_slots;
RESET ROLE;

-- cleanup
SELECT pg_drop_replication_slot('regression_slot');

DROP ROLE lr_normal;
DROP ROLE lr_superuser;
DROP ROLE lr_replication;
DROP TABLE lr_test;
