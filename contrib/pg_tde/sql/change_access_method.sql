\! rm -f '/tmp/pg_tde_test_keyring.per'

CREATE EXTENSION IF NOT EXISTS pg_tde;

SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_key_using_database_key_provider('test-db-key', 'file-vault');

CREATE TABLE country_table (
     country_id   serial primary key,
     country_name varchar(32) unique not null,
     continent    varchar(32) not null
) USING tde_heap;

INSERT INTO country_table (country_name, continent)
     VALUES ('Japan', 'Asia'),
            ('UK', 'Europe'),
            ('USA', 'North America');

SELECT * FROM country_table;
SELECT pg_tde_is_encrypted('country_table');
SELECT pg_tde_is_encrypted('country_table_country_id_seq');
SELECT pg_tde_is_encrypted('country_table_pkey');

-- Try changing the encrypted table to an unencrypted table
ALTER TABLE country_table SET ACCESS METHOD heap;

-- Insert some more data
INSERT INTO country_table (country_name, continent)
     VALUES ('France', 'Europe'),
            ('Germany', 'Europe'),
            ('Canada', 'North America');

SELECT * FROM country_table;
SELECT pg_tde_is_encrypted('country_table');
SELECT pg_tde_is_encrypted('country_table_country_id_seq');
SELECT pg_tde_is_encrypted('country_table_pkey');

-- Change it back to encrypted
ALTER TABLE country_table SET ACCESS METHOD tde_heap;

INSERT INTO country_table (country_name, continent)
     VALUES ('China', 'Asia'),
            ('Brazil', 'South America'),
            ('Australia', 'Oceania');

SELECT * FROM country_table;
SELECT pg_tde_is_encrypted('country_table');
SELECT pg_tde_is_encrypted('country_table_country_id_seq');
SELECT pg_tde_is_encrypted('country_table_pkey');

-- Test that we honor the default value
SET default_table_access_method = 'heap';

ALTER TABLE country_table SET ACCESS METHOD DEFAULT;

SELECT pg_tde_is_encrypted('country_table');

SET default_table_access_method = 'tde_heap';

ALTER TABLE country_table SET ACCESS METHOD DEFAULT;

SELECT pg_tde_is_encrypted('country_table');

RESET default_table_access_method;

ALTER TABLE country_table ADD y text;

SELECT pg_tde_is_encrypted('pg_toast.pg_toast_' || 'country_table'::regclass::oid);

CREATE TABLE country_table2 (
     country_id   serial primary key,
     country_name text unique not null,
     continent    text not null
);

SET pg_tde.enforce_encryption = on;

CREATE TABLE country_table3 (
     country_id   serial primary key,
     country_name text unique not null,
     continent    text not null
) USING heap;

ALTER TABLE country_table SET ACCESS METHOD heap;
ALTER TABLE country_table2 SET ACCESS METHOD tde_heap;

CREATE TABLE country_table3 (
     country_id   serial primary key,
     country_name text unique not null,
     continent    text not null
) USING tde_heap;

DROP TABLE country_table;
DROP TABLE country_table2;
DROP TABLE country_table3;

SET pg_tde.enforce_encryption = off;

DROP EXTENSION pg_tde;
