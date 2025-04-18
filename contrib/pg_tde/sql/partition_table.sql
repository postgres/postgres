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

DROP TABLE partitioned_table;
DROP EXTENSION pg_tde;
