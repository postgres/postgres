/* contrib/pg_tde/pg_tde--1.0-beta2.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_tde" to load this file. \quit

CREATE type PG_TDE_GLOBAL AS ENUM('PG_TDE_GLOBAL');

-- Key Provider Management
CREATE FUNCTION pg_tde_add_key_provider(provider_type VARCHAR(10), provider_name VARCHAR(128), options JSON)
RETURNS INT
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_add_key_provider_file(provider_name VARCHAR(128), file_path TEXT)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE COALESCE(file_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_add_key_provider_file(provider_name VARCHAR(128), file_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE file_path));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_add_key_provider_vault_v2(provider_name VARCHAR(128),
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
                            'url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_add_key_provider_vault_v2(provider_name VARCHAR(128),
                                                vault_token JSON,
                                                vault_url JSON,
                                                vault_mount_path JSON,
                                                vault_ca_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_add_key_provider_kmip(provider_name VARCHAR(128),
                                             kmip_host TEXT,
                                             kmip_port INT,
                                             kmip_ca_path TEXT,
                                             kmip_cert_path TEXT)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_add_key_provider_kmip(provider_name VARCHAR(128),
                                             kmip_host JSON,
                                             kmip_port JSON,
                                             kmip_ca_path JSON,
                                             kmip_cert_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_list_all_key_providers
    (OUT id INT,
    OUT provider_name VARCHAR(128),
    OUT provider_type VARCHAR(10),
    OUT options JSON)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION pg_tde_list_all_key_providers
    (PG_TDE_GLOBAL, OUT id INT,
    OUT provider_name VARCHAR(128),
    OUT provider_type VARCHAR(10),
    OUT options JSON)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Global Tablespace Key Provider Management
CREATE FUNCTION pg_tde_add_key_provider(PG_TDE_GLOBAL, provider_type VARCHAR(10), provider_name VARCHAR(128), options JSON)
RETURNS INT
AS 'MODULE_PATHNAME', 'pg_tde_add_key_provider_global'
LANGUAGE C;

CREATE FUNCTION pg_tde_add_key_provider_file(PG_TDE_GLOBAL, provider_name VARCHAR(128), file_path TEXT)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE COALESCE(file_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_add_key_provider_file(PG_TDE_GLOBAL, provider_name VARCHAR(128), file_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE file_path));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_add_key_provider_vault_v2(PG_TDE_GLOBAL,
                                                 provider_name VARCHAR(128),
                                                 vault_token TEXT,
                                                 vault_url TEXT,
                                                 vault_mount_path TEXT,
                                                 vault_ca_path TEXT)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_add_key_provider_vault_v2(PG_TDE_GLOBAL,
                                                 provider_name VARCHAR(128),
                                                 vault_token JSON,
                                                 vault_url JSON,
                                                 vault_mount_path JSON,
                                                 vault_ca_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_add_key_provider_kmip(PG_TDE_GLOBAL,
                                             provider_name VARCHAR(128),
                                             kmip_host TEXT,
                                             kmip_port INT,
                                             kmip_ca_path TEXT,
                                             kmip_cert_path TEXT)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_add_key_provider_kmip(PG_TDE_GLOBAL,
                                                        provider_name VARCHAR(128),
                                                        kmip_host JSON,
                                                        kmip_port JSON,
                                                        kmip_ca_path JSON,
                                                        kmip_cert_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
$$
LANGUAGE SQL;

-- Key Provider Management
CREATE FUNCTION pg_tde_change_key_provider(provider_type VARCHAR(10), provider_name VARCHAR(128), options JSON)
RETURNS INT
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_change_key_provider_file(provider_name VARCHAR(128), file_path TEXT)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE COALESCE(file_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_change_key_provider_file(provider_name VARCHAR(128), file_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE file_path));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_change_key_provider_vault_v2(provider_name VARCHAR(128),
                                                    vault_token TEXT,
                                                    vault_url TEXT,
                                                    vault_mount_path TEXT,
                                                    vault_ca_path TEXT)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_change_key_provider_vault_v2(provider_name VARCHAR(128),
                                                    vault_token JSON,
                                                    vault_url JSON,
                                                    vault_mount_path JSON,
                                                    vault_ca_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_change_key_provider_kmip(provider_name VARCHAR(128),
                                                kmip_host TEXT,
                                                kmip_port INT,
                                                kmip_ca_path TEXT,
                                                kmip_cert_path TEXT)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_change_key_provider_kmip(provider_name VARCHAR(128),
                                                kmip_host JSON,
                                                kmip_port JSON,
                                                kmip_ca_path JSON,
                                                kmip_cert_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
$$
LANGUAGE SQL;

-- Global Tablespace Key Provider Management
CREATE FUNCTION pg_tde_change_key_provider(PG_TDE_GLOBAL, provider_type VARCHAR(10), provider_name VARCHAR(128), options JSON)
RETURNS INT
AS 'MODULE_PATHNAME', 'pg_tde_change_key_provider_global'
LANGUAGE C;

CREATE FUNCTION pg_tde_change_key_provider_file(PG_TDE_GLOBAL, provider_name VARCHAR(128), file_path TEXT)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE COALESCE(file_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_change_key_provider_file(PG_TDE_GLOBAL, provider_name VARCHAR(128), file_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE file_path));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_change_key_provider_vault_v2(PG_TDE_GLOBAL,
                                                    provider_name VARCHAR(128),
                                                    vault_token TEXT,
                                                    vault_url TEXT,
                                                    vault_mount_path TEXT,
                                                    vault_ca_path TEXT)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_change_key_provider_vault_v2(PG_TDE_GLOBAL,
                                                    provider_name VARCHAR(128),
                                                    vault_token JSON,
                                                    vault_url JSON,
                                                    vault_mount_path JSON,
                                                    vault_ca_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_change_key_provider_kmip(PG_TDE_GLOBAL,
                                                provider_name VARCHAR(128),
                                                kmip_host TEXT,
                                                kmip_port INT,
                                                kmip_ca_path TEXT,
                                                kmip_cert_path TEXT)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_change_key_provider_kmip(PG_TDE_GLOBAL,
                                                provider_name VARCHAR(128),
                                                kmip_host JSON,
                                                kmip_port JSON,
                                                kmip_ca_path JSON,
                                                kmip_cert_path JSON)
RETURNS INT
AS $$
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
$$
LANGUAGE SQL;

-- Table access method
CREATE FUNCTION pg_tdeam_basic_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_internal_has_key(oid OID)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_is_encrypted(table_name VARCHAR)
RETURNS boolean
AS $$
    SELECT EXISTS (
        SELECT 1
        FROM   pg_catalog.pg_class
        WHERE  oid = table_name::regclass::oid
        AND    (relam = (SELECT oid FROM pg_catalog.pg_am WHERE amname = 'tde_heap_basic')
            OR (relam = (SELECT oid FROM pg_catalog.pg_am WHERE amname = 'tde_heap'))
                AND pg_tde_internal_has_key(table_name::regclass::oid))
        )
$$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_set_principal_key(principal_key_name VARCHAR(255), provider_name VARCHAR(255) DEFAULT NULL, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_set_principal_key(principal_key_name VARCHAR(255), PG_TDE_GLOBAL, provider_name VARCHAR(255) DEFAULT NULL, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_tde_set_principal_key_global'
LANGUAGE C;

CREATE FUNCTION pg_tde_set_server_principal_key(principal_key_name VARCHAR(255), PG_TDE_GLOBAL, provider_name VARCHAR(255) DEFAULT NULL, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_tde_set_principal_key_server'
LANGUAGE C;

CREATE FUNCTION pg_tde_create_wal_key()
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_extension_initialize()
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_verify_principal_key()
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_verify_global_principal_key()
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_principal_key_info()
RETURNS TABLE ( principal_key_name text,
                key_provider_name text,
                key_provider_id integer,
                key_createion_time timestamp with time zone)
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_principal_key_info(PG_TDE_GLOBAL)
RETURNS TABLE ( principal_key_name text,
                key_provider_name text,
                key_provider_id integer,
                key_createion_time timestamp with time zone)
AS 'MODULE_PATHNAME', 'pg_tde_principal_key_info_global'
LANGUAGE C;

CREATE FUNCTION pg_tde_delete_key_provider(PG_TDE_GLOBAL, provider_name VARCHAR)
RETURNS VOID
AS 'MODULE_PATHNAME', 'pg_tde_delete_key_provider_global'
LANGUAGE C;

CREATE FUNCTION pg_tde_delete_key_provider(provider_name VARCHAR)
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_version() RETURNS TEXT AS 'MODULE_PATHNAME' LANGUAGE C;

-- Access method
CREATE ACCESS METHOD tde_heap_basic TYPE TABLE HANDLER pg_tdeam_basic_handler;
COMMENT ON ACCESS METHOD tde_heap_basic IS 'pg_tde table access method';

DO $$
    BEGIN
        -- Table access method
        CREATE FUNCTION pg_tdeam_handler(internal)
        RETURNS table_am_handler
        AS 'MODULE_PATHNAME'
        LANGUAGE C;

        CREATE ACCESS METHOD tde_heap TYPE TABLE HANDLER pg_tdeam_handler;
        COMMENT ON ACCESS METHOD tde_heap IS 'tde_heap table access method';

        CREATE FUNCTION pg_tde_ddl_command_start_capture()
        RETURNS event_trigger
        AS 'MODULE_PATHNAME'
        LANGUAGE C;

        CREATE FUNCTION pg_tde_ddl_command_end_capture()
        RETURNS event_trigger
        AS 'MODULE_PATHNAME'
        LANGUAGE C;

        CREATE EVENT TRIGGER pg_tde_trigger_create_index
        ON ddl_command_start
        EXECUTE FUNCTION pg_tde_ddl_command_start_capture();
        ALTER EVENT TRIGGER pg_tde_trigger_create_index ENABLE ALWAYS;

        CREATE EVENT TRIGGER pg_tde_trigger_create_index_2
        ON ddl_command_end
        EXECUTE FUNCTION pg_tde_ddl_command_end_capture();
        ALTER EVENT TRIGGER pg_tde_trigger_create_index_2 ENABLE ALWAYS;
    EXCEPTION WHEN OTHERS THEN
    END;
$$;

-- Per database extension initialization
SELECT pg_tde_extension_initialize();

CREATE FUNCTION pg_tde_grant_execute_privilege_on_function(
    target_user_or_role TEXT,
    target_function_name TEXT,
    target_function_args TEXT
)
RETURNS VOID AS $$
DECLARE
    grant_query TEXT;
BEGIN
    -- Construct the GRANT statement
    grant_query := format('GRANT EXECUTE ON FUNCTION %I(%s) TO %I;',
                          target_function_name, target_function_args, target_user_or_role);

    -- Execute the GRANT statement
    EXECUTE grant_query;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION pg_tde_revoke_execute_privilege_on_function(
    target_user_or_role TEXT,
    target_function_name TEXT,
    argument_types TEXT
)
RETURNS VOID AS $$
DECLARE
    revoke_query TEXT;
BEGIN
    -- Construct the REVOKE statement
    revoke_query := format('REVOKE EXECUTE ON FUNCTION %I(%s) FROM %I;',
                           target_function_name, argument_types, target_user_or_role);

    -- Execute the REVOKE statement
    EXECUTE revoke_query;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION pg_tde_grant_global_key_management_to_role(
    target_user_or_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider', 'pg_tde_global, varchar, varchar, JSON');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_file', 'pg_tde_global, varchar, json');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_file', 'pg_tde_global, varchar, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_vault_v2', 'pg_tde_global, varchar, text, text, text, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_vault_v2', 'pg_tde_global, varchar, JSON, JSON, JSON, JSON');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_kmip', 'pg_tde_global, varchar, text, int, text, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_kmip', 'pg_tde_global, varchar, JSON, JSON, JSON, JSON');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider', 'pg_tde_global, varchar, varchar, JSON');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_file', 'pg_tde_global, varchar, json');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_file', 'pg_tde_global, varchar, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_vault_v2', 'pg_tde_global, varchar, text, text, text, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_vault_v2', 'pg_tde_global, varchar, JSON, JSON, JSON, JSON');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_kmip', 'pg_tde_global, varchar, text, int, text, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_kmip', 'pg_tde_global, varchar, JSON, JSON, JSON, JSON');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_delete_key_provider', 'pg_tde_global, varchar');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_set_principal_key', 'varchar, pg_tde_global, varchar, BOOLEAN');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_set_server_principal_key', 'varchar, pg_tde_global, varchar, BOOLEAN');
END;
$$;

CREATE FUNCTION pg_tde_grant_local_key_management_to_role(
    target_user_or_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider', 'varchar, varchar, JSON');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_file', 'varchar, json');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_file', 'varchar, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_vault_v2', 'varchar, text, text, text, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_vault_v2', 'varchar, JSON, JSON, JSON, JSON');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_kmip', 'varchar, text, int, text, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_kmip', 'varchar, JSON, JSON, JSON, JSON');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider', 'varchar, varchar, JSON');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_file', 'varchar, json');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_file', 'varchar, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_vault_v2', 'varchar, text, text, text, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_vault_v2', 'varchar, JSON, JSON, JSON, JSON');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_kmip', 'varchar, text, int, text, text');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_kmip', 'varchar, JSON, JSON, JSON, JSON');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_delete_key_provider', 'varchar');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_set_principal_key', 'varchar, varchar, BOOLEAN');
END;
$$;

CREATE FUNCTION pg_tde_grant_key_viewer_to_role(
    target_user_or_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_list_all_key_providers', 'OUT INT, OUT varchar, OUT varchar, OUT JSON');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_list_all_key_providers', 'pg_tde_global, OUT INT, OUT varchar, OUT varchar, OUT JSON');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_is_encrypted', 'VARCHAR');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_principal_key_info', '');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_principal_key_info', 'pg_tde_global');
END;
$$;

CREATE FUNCTION pg_tde_revoke_global_key_management_from_role(
    target_user_or_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider', 'pg_tde_global, varchar, varchar, JSON');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_file', 'pg_tde_global, varchar, json');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_file', 'pg_tde_global, varchar, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_vault_v2', 'pg_tde_global, varchar, text, text, text, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_vault_v2', 'pg_tde_global, varchar, JSON, JSON, JSON, JSON');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_kmip', 'pg_tde_global, varchar, text, int, text, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_kmip', 'pg_tde_global, varchar, JSON, JSON, JSON, JSON');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider', 'pg_tde_global, varchar, varchar, JSON');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_file', 'pg_tde_global, varchar, json');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_file', 'pg_tde_global, varchar, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_vault_v2', 'pg_tde_global, varchar, text, text, text, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_vault_v2', 'pg_tde_global, varchar, JSON, JSON, JSON, JSON');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_kmip', 'pg_tde_global, varchar, text, int, text, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_kmip', 'pg_tde_global, varchar, JSON, JSON, JSON, JSON');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_delete_key_provider', 'pg_tde_global, varchar');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_set_principal_key', 'varchar, pg_tde_global, varchar, BOOLEAN');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_set_server_principal_key', 'varchar, pg_tde_global, varchar, BOOLEAN');
END;
$$;

CREATE FUNCTION pg_tde_revoke_local_key_management_from_role(
    target_user_or_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider', 'varchar, varchar, JSON');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_file', 'varchar, json');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_file', 'varchar, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_vault_v2', 'varchar, text, text, text, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_vault_v2', 'varchar, JSON, JSON, JSON, JSON');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_kmip', 'varchar, text, int, text, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_add_key_provider_kmip', 'varchar, JSON, JSON, JSON, JSON');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider', 'varchar, varchar, JSON');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_file', 'varchar, json');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_file', 'varchar, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_vault_v2', 'varchar, text, text, text, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_vault_v2', 'varchar, JSON, JSON, JSON, JSON');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_kmip', 'varchar, text, int, text, text');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_change_key_provider_kmip', 'varchar, JSON, JSON, JSON, JSON');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_delete_key_provider', 'varchar');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_set_principal_key', 'varchar, varchar, BOOLEAN');
END;
$$;

CREATE FUNCTION pg_tde_revoke_key_viewer_from_role(
    target_user_or_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_list_all_key_providers', 'OUT INT, OUT varchar, OUT varchar, OUT JSON');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_list_all_key_providers', 'pg_tde_global, OUT INT, OUT varchar, OUT varchar, OUT JSON');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_is_encrypted', 'VARCHAR');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_principal_key_info', '');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_principal_key_info', 'pg_tde_global');
END;
$$;

CREATE FUNCTION pg_tde_grant_grant_management_to_role(
    target_user_or_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_grant_global_key_management_to_role', 'TEXT');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_grant_local_key_management_to_role', 'TEXT');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_grant_grant_management_to_role', 'TEXT');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_grant_key_viewer_to_role', 'TEXT');

    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_revoke_global_key_management_from_role', 'TEXT');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_revoke_local_key_management_from_role', 'TEXT');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_revoke_grant_management_from_role', 'TEXT');
    PERFORM pg_tde_grant_execute_privilege_on_function(target_user_or_role, 'pg_tde_revoke_key_viewer_from_role', 'TEXT');
END;
$$;

CREATE FUNCTION pg_tde_revoke_grant_management_from_role(
    target_user_or_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_grant_global_key_management_to_role', 'TEXT');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_grant_local_key_management_to_role', 'TEXT');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_grant_grant_management_to_role', 'TEXT');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_grant_key_viewer_to_role', 'TEXT');

    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_revoke_global_key_management_from_role', 'TEXT');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_revoke_local_key_management_from_role', 'TEXT');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_revoke_grant_management_from_role', 'TEXT');
    PERFORM pg_tde_revoke_execute_privilege_on_function(target_user_or_role, 'pg_tde_revoke_key_viewer_from_role', 'TEXT');
END;
$$;

-- Revoking all the privileges from the public role
SELECT pg_tde_revoke_local_key_management_from_role('public');
SELECT pg_tde_revoke_global_key_management_from_role('public');
SELECT pg_tde_revoke_grant_management_from_role('public');
SELECT pg_tde_revoke_key_viewer_from_role('public');
