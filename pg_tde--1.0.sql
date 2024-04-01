/* contrib/pg_tde/pg_tde--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_tde" to load this file. \quit

-- pg_tde catalog tables
CREATE SCHEMA percona_tde;
-- Note: The table is created using heap storage becasue we do not want this table
-- to be encrypted by pg_tde. This table is used to store key provider information
-- and we do not want to encrypt this table using pg_tde.
CREATE TABLE percona_tde.pg_tde_key_provider(provider_id SERIAL,
        keyring_type VARCHAR(10) CHECK (keyring_type IN ('file', 'vault-v2')),
        provider_name VARCHAR(255) UNIQUE NOT NULL, options JSON, PRIMARY KEY(provider_id)) using heap;

-- If you want to add new provider types, you need to make appropriate changes
-- in include/catalog/tde_keyring.h and src/catalog/tde_keyring.c files.

SELECT pg_catalog.pg_extension_config_dump('percona_tde.pg_tde_key_provider', '');

-- Trigger function to check master key dependency on key provider row
CREATE FUNCTION keyring_delete_dependency_check_trigger()
RETURNS TRIGGER
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE TRIGGER pg_tde_key_provider_delete_dependency_check_trigger
BEFORE DELETE ON percona_tde.pg_tde_key_provider
FOR EACH ROW
EXECUTE FUNCTION keyring_delete_dependency_check_trigger();

-- Key Provider Management

CREATE OR REPLACE FUNCTION pg_tde_add_key_provider(provider_type VARCHAR(10), provider_name VARCHAR(128), options JSON)
RETURNS INT
AS $$
    INSERT INTO percona_tde.pg_tde_key_provider (keyring_type, provider_name, options) VALUES (provider_type, provider_name, options) RETURNING provider_id;
$$
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION pg_tde_add_key_provider_file(provider_name VARCHAR(128), file_path TEXT)
RETURNS INT
AS $$
-- JSON keys in the options must be matched to the keys in
-- load_file_keyring_provider_options function.

    SELECT pg_tde_add_key_provider('file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE COALESCE(file_path, '')));
$$
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION pg_tde_add_key_provider_file(provider_name VARCHAR(128), file_path JSON)
RETURNS INT
AS $$
-- JSON keys in the options must be matched to the keys in
-- load_file_keyring_provider_options function.

    SELECT pg_tde_add_key_provider('file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE file_path));
$$
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION pg_tde_add_key_provider_vault_v2(provider_name VARCHAR(128),
                                                        vault_token TEXT,
                                                        vault_url TEXT,
                                                        vault_mount_path TEXT,
                                                        vault_ca_path TEXT)
RETURNS INT
AS $$
-- JSON keys in the options must be matched to the keys in
-- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE COALESCE(vault_url,''),
                            'token' VALUE COALESCE(vault_token,''),
                            'mountPath' VALUE COALESCE(vault_mount_path,''),
                            'caPath' VALUE COALESCE(vault_ca_path,'')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_get_keyprovider(provider_name text)
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;
-- Table access method
CREATE FUNCTION pg_tdeam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pgtde_is_encrypted(table_name VARCHAR)
RETURNS boolean
AS $$
SELECT EXISTS (
    SELECT 1
    FROM   pg_catalog.pg_class
    WHERE  relname = table_name
    AND    relam = (SELECT oid FROM pg_catalog.pg_am WHERE amname = 'pg_tde')
    )$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_rotate_key(new_master_key_name VARCHAR(255) DEFAULT NULL, new_provider_name VARCHAR(255) DEFAULT NULL, ensure_new_key BOOLEAN DEFAULT TRUE)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_set_master_key(master_key_name VARCHAR(255), provider_name VARCHAR(255), ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_extension_initialize()
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_master_key_info()
RETURNS TABLE ( master_key_name text,
                key_provider_name text,
                key_provider_id integer,
                master_key_internal_name text,
                master_key_version integer,
                key_createion_time timestamp with time zone)
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_version() RETURNS TEXT AS 'MODULE_PATHNAME' LANGUAGE C;

-- Access method
CREATE ACCESS METHOD pg_tde TYPE TABLE HANDLER pg_tdeam_handler;
COMMENT ON ACCESS METHOD pg_tde IS 'pg_tde table access method';

-- Per database extension initialization
SELECT pg_tde_extension_initialize();
