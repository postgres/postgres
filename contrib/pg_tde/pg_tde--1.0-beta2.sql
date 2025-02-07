/* contrib/pg_tde/pg_tde--1.0-beta2.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_tde" to load this file. \quit

CREATE type PG_TDE_GLOBAL AS ENUM('PG_TDE_GLOBAL');

-- Key Provider Management
CREATE FUNCTION pg_tde_add_key_provider(provider_type VARCHAR(10), provider_name VARCHAR(128), options JSON)
RETURNS INT
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_add_key_provider_file(provider_name VARCHAR(128), file_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE COALESCE(file_path, '')));
END;

CREATE FUNCTION pg_tde_add_key_provider_file(provider_name VARCHAR(128), file_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_add_key_provider_vault_v2(provider_name VARCHAR(128),
                                                vault_token TEXT,
                                                vault_url TEXT,
                                                vault_mount_path TEXT,
                                                vault_ca_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
END;

CREATE FUNCTION pg_tde_add_key_provider_vault_v2(provider_name VARCHAR(128),
                                                vault_token JSON,
                                                vault_url JSON,
                                                vault_mount_path JSON,
                                                vault_ca_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_add_key_provider_kmip(provider_name VARCHAR(128),
                                             kmip_host TEXT,
                                             kmip_port INT,
                                             kmip_ca_path TEXT,
                                             kmip_cert_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
END;

CREATE FUNCTION pg_tde_add_key_provider_kmip(provider_name VARCHAR(128),
                                             kmip_host JSON,
                                             kmip_port JSON,
                                             kmip_ca_path JSON,
                                             kmip_cert_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
END;

CREATE FUNCTION pg_tde_list_all_key_providers
    (OUT id INT,
    OUT provider_name VARCHAR(128),
    OUT provider_type VARCHAR(10),
    OUT options JSON)
RETURNS SETOF record
LANGUAGE C STRICT
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_list_all_key_providers
    (PG_TDE_GLOBAL, OUT id INT,
    OUT provider_name VARCHAR(128),
    OUT provider_type VARCHAR(10),
    OUT options JSON)
RETURNS SETOF record
LANGUAGE C STRICT
AS 'MODULE_PATHNAME';

-- Global Tablespace Key Provider Management
CREATE FUNCTION pg_tde_add_key_provider(PG_TDE_GLOBAL, provider_type VARCHAR(10), provider_name VARCHAR(128), options JSON)
RETURNS INT
LANGUAGE C
AS 'MODULE_PATHNAME', 'pg_tde_add_key_provider_global';

CREATE FUNCTION pg_tde_add_key_provider_file(PG_TDE_GLOBAL, provider_name VARCHAR(128), file_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE COALESCE(file_path, '')));
END;

CREATE FUNCTION pg_tde_add_key_provider_file(PG_TDE_GLOBAL, provider_name VARCHAR(128), file_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_add_key_provider_vault_v2(PG_TDE_GLOBAL,
                                                 provider_name VARCHAR(128),
                                                 vault_token TEXT,
                                                 vault_url TEXT,
                                                 vault_mount_path TEXT,
                                                 vault_ca_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
END;

CREATE FUNCTION pg_tde_add_key_provider_vault_v2(PG_TDE_GLOBAL,
                                                 provider_name VARCHAR(128),
                                                 vault_token JSON,
                                                 vault_url JSON,
                                                 vault_mount_path JSON,
                                                 vault_ca_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_add_key_provider_kmip(PG_TDE_GLOBAL,
                                             provider_name VARCHAR(128),
                                             kmip_host TEXT,
                                             kmip_port INT,
                                             kmip_ca_path TEXT,
                                             kmip_cert_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
END;

CREATE FUNCTION pg_tde_add_key_provider_kmip(PG_TDE_GLOBAL,
                                                        provider_name VARCHAR(128),
                                                        kmip_host JSON,
                                                        kmip_port JSON,
                                                        kmip_ca_path JSON,
                                                        kmip_cert_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
END;

-- Key Provider Management
CREATE FUNCTION pg_tde_change_key_provider(provider_type VARCHAR(10), provider_name VARCHAR(128), options JSON)
RETURNS INT
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_change_key_provider_file(provider_name VARCHAR(128), file_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE COALESCE(file_path, '')));
END;

CREATE FUNCTION pg_tde_change_key_provider_file(provider_name VARCHAR(128), file_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_change_key_provider_vault_v2(provider_name VARCHAR(128),
                                                    vault_token TEXT,
                                                    vault_url TEXT,
                                                    vault_mount_path TEXT,
                                                    vault_ca_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
END;

CREATE FUNCTION pg_tde_change_key_provider_vault_v2(provider_name VARCHAR(128),
                                                    vault_token JSON,
                                                    vault_url JSON,
                                                    vault_mount_path JSON,
                                                    vault_ca_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_change_key_provider_kmip(provider_name VARCHAR(128),
                                                kmip_host TEXT,
                                                kmip_port INT,
                                                kmip_ca_path TEXT,
                                                kmip_cert_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
END;

CREATE FUNCTION pg_tde_change_key_provider_kmip(provider_name VARCHAR(128),
                                                kmip_host JSON,
                                                kmip_port JSON,
                                                kmip_ca_path JSON,
                                                kmip_cert_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
END;

-- Global Tablespace Key Provider Management
CREATE FUNCTION pg_tde_change_key_provider(PG_TDE_GLOBAL, provider_type VARCHAR(10), provider_name VARCHAR(128), options JSON)
RETURNS INT
LANGUAGE C
AS 'MODULE_PATHNAME', 'pg_tde_change_key_provider_global';

CREATE FUNCTION pg_tde_change_key_provider_file(PG_TDE_GLOBAL, provider_name VARCHAR(128), file_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE COALESCE(file_path, '')));
END;

CREATE FUNCTION pg_tde_change_key_provider_file(PG_TDE_GLOBAL, provider_name VARCHAR(128), file_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'file', provider_name,
                json_object('type' VALUE 'file', 'path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_change_key_provider_vault_v2(PG_TDE_GLOBAL,
                                                    provider_name VARCHAR(128),
                                                    vault_token TEXT,
                                                    vault_url TEXT,
                                                    vault_mount_path TEXT,
                                                    vault_ca_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
END;

CREATE FUNCTION pg_tde_change_key_provider_vault_v2(PG_TDE_GLOBAL,
                                                    provider_name VARCHAR(128),
                                                    vault_token JSON,
                                                    vault_url JSON,
                                                    vault_mount_path JSON,
                                                    vault_ca_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_change_key_provider_kmip(PG_TDE_GLOBAL,
                                                provider_name VARCHAR(128),
                                                kmip_host TEXT,
                                                kmip_port INT,
                                                kmip_ca_path TEXT,
                                                kmip_cert_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'kmip', provider_name,
                            json_object('type' VALUE 'kmip',
                            'host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
END;

CREATE FUNCTION pg_tde_change_key_provider_kmip(PG_TDE_GLOBAL,
                                                provider_name VARCHAR(128),
                                                kmip_host JSON,
                                                kmip_port JSON,
                                                kmip_ca_path JSON,
                                                kmip_cert_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_key_provider('PG_TDE_GLOBAL', 'vault-v2', provider_name,
                            json_object('type' VALUE 'vault-v2',
                            'host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
END;

-- Table access method
CREATE FUNCTION pg_tdeam_basic_handler(internal)
RETURNS table_am_handler
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_internal_has_key(oid OID)
RETURNS boolean
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_is_encrypted(table_name VARCHAR)
RETURNS boolean
LANGUAGE SQL
BEGIN ATOMIC
    SELECT EXISTS (
        SELECT 1
        FROM   pg_catalog.pg_class
        WHERE  oid = table_name::regclass::oid
        AND    (relam = (SELECT oid FROM pg_catalog.pg_am WHERE amname = 'tde_heap_basic')
            OR (relam = (SELECT oid FROM pg_catalog.pg_am WHERE amname = 'tde_heap'))
                AND pg_tde_internal_has_key(table_name::regclass::oid))
        );
END;

CREATE FUNCTION pg_tde_set_principal_key(principal_key_name VARCHAR(255), provider_name VARCHAR(255) DEFAULT NULL, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS boolean
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_set_principal_key(principal_key_name VARCHAR(255), PG_TDE_GLOBAL, provider_name VARCHAR(255) DEFAULT NULL, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS boolean
LANGUAGE C
AS 'MODULE_PATHNAME', 'pg_tde_set_principal_key_global';

CREATE FUNCTION pg_tde_set_server_principal_key(principal_key_name VARCHAR(255), PG_TDE_GLOBAL, provider_name VARCHAR(255) DEFAULT NULL, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS boolean
LANGUAGE C
AS 'MODULE_PATHNAME', 'pg_tde_set_principal_key_server';

CREATE FUNCTION pg_tde_create_wal_key()
RETURNS boolean
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_extension_initialize()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_verify_principal_key()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_verify_global_principal_key()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_principal_key_info()
RETURNS TABLE ( principal_key_name text,
                key_provider_name text,
                key_provider_id integer,
                key_createion_time timestamp with time zone)
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_principal_key_info(PG_TDE_GLOBAL)
RETURNS TABLE ( principal_key_name text,
                key_provider_name text,
                key_provider_id integer,
                key_createion_time timestamp with time zone)
LANGUAGE C
AS 'MODULE_PATHNAME', 'pg_tde_principal_key_info_global';

CREATE FUNCTION pg_tde_delete_key_provider(PG_TDE_GLOBAL, provider_name VARCHAR)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME', 'pg_tde_delete_key_provider_global';

CREATE FUNCTION pg_tde_delete_key_provider(provider_name VARCHAR)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_version() RETURNS TEXT LANGUAGE C AS 'MODULE_PATHNAME';

-- Access method
CREATE ACCESS METHOD tde_heap_basic TYPE TABLE HANDLER pg_tdeam_basic_handler;
COMMENT ON ACCESS METHOD tde_heap_basic IS 'pg_tde table access method';

DO $$
    BEGIN
        -- Table access method
        CREATE FUNCTION pg_tdeam_handler(internal)
        RETURNS table_am_handler
        LANGUAGE C
        AS 'MODULE_PATHNAME';

        CREATE ACCESS METHOD tde_heap TYPE TABLE HANDLER pg_tdeam_handler;
        COMMENT ON ACCESS METHOD tde_heap IS 'tde_heap table access method';

        CREATE FUNCTION pg_tde_ddl_command_start_capture()
        RETURNS event_trigger
        LANGUAGE C
        AS 'MODULE_PATHNAME';

        CREATE FUNCTION pg_tde_ddl_command_end_capture()
        RETURNS event_trigger
        LANGUAGE C
        AS 'MODULE_PATHNAME';

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

CREATE FUNCTION pg_tde_grant_global_key_management_to_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider(pg_tde_global, varchar, varchar, JSON) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_file(pg_tde_global, varchar, json) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_file(pg_tde_global, varchar, text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_vault_v2(pg_tde_global, varchar, text, text, text, text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_vault_v2(pg_tde_global, varchar, JSON, JSON, JSON, JSON) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_kmip(pg_tde_global, varchar, text, int, text, text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_kmip(pg_tde_global, varchar, JSON, JSON, JSON, JSON) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider(pg_tde_global, varchar, varchar, JSON) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_file(pg_tde_global, varchar, json) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_file(pg_tde_global, varchar, text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_vault_v2(pg_tde_global, varchar, text, text, text, text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_vault_v2(pg_tde_global, varchar, JSON, JSON, JSON, JSON) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_kmip(pg_tde_global, varchar, text, int, text, text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_kmip(pg_tde_global, varchar, JSON, JSON, JSON, JSON) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_delete_key_provider(pg_tde_global, varchar) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_set_principal_key(varchar, pg_tde_global, varchar, BOOLEAN) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_set_server_principal_key(varchar, pg_tde_global, varchar, BOOLEAN) TO %I', target_role);
END;
$$;

CREATE FUNCTION pg_tde_grant_local_key_management_to_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider(varchar, varchar, JSON) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_file(varchar, json) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_file(varchar, text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_vault_v2(varchar, text, text, text, text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_vault_v2(varchar, JSON, JSON, JSON, JSON) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_kmip(varchar, text, int, text, text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_add_key_provider_kmip(varchar, JSON, JSON, JSON, JSON) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider(varchar, varchar, JSON) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_file(varchar, json) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_file(varchar, text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_vault_v2(varchar, text, text,text,text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_vault_v2(varchar, JSON, JSON,JSON,JSON) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_kmip(varchar, text, int, text, text) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_change_key_provider_kmip(varchar, JSON, JSON, JSON, JSON) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_delete_key_provider(varchar) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_set_principal_key(varchar, varchar, BOOLEAN) TO %I', target_role);
END;
$$;

CREATE FUNCTION pg_tde_grant_key_viewer_to_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_list_all_key_providers(OUT INT, OUT varchar, OUT varchar, OUT JSON) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_list_all_key_providers(pg_tde_global, OUT INT, OUT varchar, OUT varchar, OUT JSON) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_is_encrypted(VARCHAR) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_principal_key_info() TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_principal_key_info(pg_tde_global) TO %I', target_role);
END;
$$;

CREATE FUNCTION pg_tde_revoke_global_key_management_from_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider(pg_tde_global, varchar, varchar, JSON) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_file(pg_tde_global, varchar, json) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_file(pg_tde_global, varchar, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_vault_v2(pg_tde_global, varchar, text, text, text, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_vault_v2(pg_tde_global, varchar, JSON, JSON, JSON, JSON) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_kmip(pg_tde_global, varchar, text, int, text, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_kmip(pg_tde_global, varchar, JSON, JSON, JSON, JSON) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider(pg_tde_global, varchar, varchar, JSON) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_file(pg_tde_global, varchar, json) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_file(pg_tde_global, varchar, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_vault_v2(pg_tde_global, varchar, text, text, text, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_vault_v2(pg_tde_global, varchar, JSON, JSON, JSON, JSON) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_kmip(pg_tde_global, varchar, text, int, text, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_kmip(pg_tde_global, varchar, JSON, JSON, JSON, JSON) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_delete_key_provider(pg_tde_global, varchar) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_set_principal_key(varchar, pg_tde_global, varchar, BOOLEAN) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_set_server_principal_key(varchar, pg_tde_global, varchar, BOOLEAN) FROM %I', target_role);
END;
$$;

CREATE FUNCTION pg_tde_revoke_local_key_management_from_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider(varchar, varchar, JSON) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_file(varchar, json) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_file(varchar, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_vault_v2(varchar, text, text, text, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_vault_v2(varchar, JSON, JSON, JSON, JSON) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_kmip(varchar, text, int, text, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_add_key_provider_kmip(varchar, JSON, JSON, JSON, JSON) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider(varchar, varchar, JSON) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_file(varchar, json) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_file(varchar, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_vault_v2(varchar, text, text, text, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_vault_v2(varchar, JSON, JSON, JSON, JSON) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_kmip(varchar, text, int, text, text) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_change_key_provider_kmip(varchar, JSON, JSON, JSON, JSON) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_delete_key_provider(varchar) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_set_principal_key(varchar, varchar, BOOLEAN) FROM %I', target_role);
END;
$$;

CREATE FUNCTION pg_tde_revoke_key_viewer_from_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_list_all_key_providers(OUT INT, OUT varchar, OUT varchar, OUT JSON) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_list_all_key_providers(pg_tde_global, OUT INT, OUT varchar, OUT varchar, OUT JSON) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_is_encrypted(VARCHAR) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_principal_key_info() FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_principal_key_info(pg_tde_global) FROM %I', target_role);
END;
$$;

CREATE FUNCTION pg_tde_grant_grant_management_to_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_grant_global_key_management_to_role(TEXT) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_grant_local_key_management_to_role(TEXT) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_grant_grant_management_to_role(TEXT) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_grant_key_viewer_to_role(TEXT) TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_revoke_global_key_management_from_role(TEXT) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_revoke_local_key_management_from_role(TEXT) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_revoke_grant_management_from_role(TEXT) TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_revoke_key_viewer_from_role(TEXT) TO %I', target_role);
END;
$$;

CREATE FUNCTION pg_tde_revoke_grant_management_from_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_grant_global_key_management_to_role(TEXT) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_grant_local_key_management_to_role(TEXT) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_grant_grant_management_to_role(TEXT) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_grant_key_viewer_to_role(TEXT) FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_revoke_global_key_management_from_role(TEXT) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_revoke_local_key_management_from_role(TEXT) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_revoke_grant_management_from_role(TEXT) FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_revoke_key_viewer_from_role(TEXT) FROM %I', target_role);
END;
$$;

-- Revoking all the privileges from the public role
SELECT pg_tde_revoke_local_key_management_from_role('public');
SELECT pg_tde_revoke_global_key_management_from_role('public');
SELECT pg_tde_revoke_grant_management_from_role('public');
SELECT pg_tde_revoke_key_viewer_from_role('public');
