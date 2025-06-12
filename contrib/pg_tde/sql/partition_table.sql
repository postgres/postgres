\! rm -f '/tmp/pg_tde_keyring.per'

CREATE EXTENSION pg_tde;
SELECT pg_tde_add_database_key_provider_file('database_keyring_provider','/tmp/pg_tde_keyring.per');
SELECT pg_tde_set_key_using_database_key_provider('table_key','database_keyring_provider');
CREATE TABLE IF NOT EXISTS partitioned_table (
    id SERIAL,
    data TEXT,
    created_at DATE NOT NULL,
    PRIMARY KEY (id, created_at)
    ) PARTITION BY RANGE (created_at) USING tde_heap;

CREATE TABLE partition_q1_2024 PARTITION OF partitioned_table FOR VALUES FROM ('2024-01-01') TO ('2024-04-01') USING tde_heap;
CREATE TABLE partition_q2_2024 PARTITION OF partitioned_table FOR VALUES FROM ('2024-04-01') TO ('2024-07-01') USING heap;
CREATE TABLE partition_q3_2024 PARTITION OF partitioned_table FOR VALUES FROM ('2024-07-01') TO ('2024-10-01') USING tde_heap;
CREATE TABLE partition_q4_2024 PARTITION OF partitioned_table FOR VALUES FROM ('2024-10-01') TO ('2025-01-01') USING heap;

SELECT pg_tde_is_encrypted('partitioned_table');
SELECT pg_tde_is_encrypted('partition_q1_2024');
SELECT pg_tde_is_encrypted('partition_q2_2024');
SELECT pg_tde_is_encrypted('partition_q3_2024');
SELECT pg_tde_is_encrypted('partition_q4_2024');

ALTER TABLE partitioned_table SET ACCESS METHOD heap;
ALTER TABLE partition_q1_2024 SET ACCESS METHOD heap;
ALTER TABLE partition_q2_2024 SET ACCESS METHOD tde_heap;
ALTER TABLE partition_q3_2024 SET ACCESS METHOD heap;
ALTER TABLE partition_q4_2024 SET ACCESS METHOD tde_heap;

SELECT pg_tde_is_encrypted('partitioned_table');
SELECT pg_tde_is_encrypted('partition_q1_2024');
SELECT pg_tde_is_encrypted('partition_q2_2024');
SELECT pg_tde_is_encrypted('partition_q3_2024');
SELECT pg_tde_is_encrypted('partition_q4_2024');

-- Does not care about parent AM as long as all children with storage use the same
ALTER TABLE partition_q1_2024 SET ACCESS METHOD tde_heap;
ALTER TABLE partition_q2_2024 SET ACCESS METHOD tde_heap;
ALTER TABLE partition_q3_2024 SET ACCESS METHOD tde_heap;
ALTER TABLE partition_q4_2024 SET ACCESS METHOD tde_heap;
ALTER TABLE partitioned_table SET ACCESS METHOD heap;

DROP TABLE partitioned_table;

-- Partition inherits encryption status from parent table if default is heap and parent is tde_heap
SET default_table_access_method = "heap";
CREATE TABLE partition_parent (a int) PARTITION BY RANGE (a) USING tde_heap;
CREATE TABLE partition_child PARTITION OF partition_parent FOR VALUES FROM (0) TO (9);
SELECT pg_tde_is_encrypted('partition_child');
DROP TABLE partition_parent;
RESET default_table_access_method;

-- Partition inherits encryption status from parent table if default is tde_heap and parent is heap
SET default_table_access_method = "tde_heap";
CREATE TABLE partition_parent (a int) PARTITION BY RANGE (a) USING heap;
CREATE TABLE partition_child PARTITION OF partition_parent FOR VALUES FROM (0) TO (9);
SELECT pg_tde_is_encrypted('partition_child');
DROP TABLE partition_parent;
RESET default_table_access_method;

-- Partition uses default access method to determine encryption status if neither parent nor child have an access method set
CREATE TABLE partition_parent (a int) PARTITION BY RANGE (a);
SET default_table_access_method = "tde_heap";
CREATE TABLE partition_child_tde PARTITION OF partition_parent FOR VALUES FROM (0) TO (9);
SELECT pg_tde_is_encrypted('partition_child_tde');
SET default_table_access_method = "heap";
CREATE TABLE partition_child_heap PARTITION OF partition_parent FOR VALUES FROM (10) TO (19);
SELECT pg_tde_is_encrypted('partition_child_heap');
DROP TABLE partition_parent;
RESET default_table_access_method;

-- Enforce encryption GUC is respected when creating partitions even if parent is plain text
CREATE TABLE partition_parent (a int) PARTITION BY RANGE (a) USING heap;
SET pg_tde.enforce_encryption = on;
CREATE TABLE partition_child_inherit PARTITION OF partition_parent FOR VALUES FROM (0) TO (10);
CREATE TABLE partition_child_heap PARTITION OF partition_parent FOR VALUES FROM (11) TO (20) USING heap;
CREATE TABLE partition_child_tde_heap PARTITION OF partition_parent FOR VALUES FROM (11) TO (20) USING tde_heap;
SELECT pg_tde_is_encrypted('partition_child_tde_heap');
DROP TABLE partition_parent;
RESET pg_tde.enforce_encryption;

DROP EXTENSION pg_tde;
