/* contrib/pg_tde/pg_tde--1.0-rc.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_tde" to load this file. \quit

-- Key Provider Management
CREATE FUNCTION pg_tde_add_database_key_provider(provider_type TEXT, provider_name TEXT, options JSON)
RETURNS INT
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_add_database_key_provider_file(provider_name TEXT, file_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_database_key_provider('file', provider_name,
                json_object('path' VALUE COALESCE(file_path, '')));
END;

CREATE FUNCTION pg_tde_add_database_key_provider_file(provider_name TEXT, file_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_database_key_provider('file', provider_name,
                json_object('path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_add_database_key_provider_vault_v2(provider_name TEXT,
                                                vault_token TEXT,
                                                vault_url TEXT,
                                                vault_mount_path TEXT,
                                                vault_ca_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_database_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
END;

CREATE FUNCTION pg_tde_add_database_key_provider_vault_v2(provider_name TEXT,
                                                vault_token JSON,
                                                vault_url JSON,
                                                vault_mount_path JSON,
                                                vault_ca_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_database_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_add_database_key_provider_kmip(provider_name TEXT,
                                             kmip_host TEXT,
                                             kmip_port INT,
                                             kmip_ca_path TEXT,
                                             kmip_cert_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_database_key_provider('kmip', provider_name,
                            json_object('host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
END;

CREATE FUNCTION pg_tde_add_database_key_provider_kmip(provider_name TEXT,
                                             kmip_host JSON,
                                             kmip_port JSON,
                                             kmip_ca_path JSON,
                                             kmip_cert_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_database_key_provider('kmip', provider_name,
                            json_object('host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
END;

CREATE FUNCTION pg_tde_list_all_database_key_providers
    (OUT id INT,
    OUT provider_name TEXT,
    OUT provider_type TEXT,
    OUT options JSON)
RETURNS SETOF record
LANGUAGE C STRICT
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_list_all_global_key_providers
    (OUT id INT,
    OUT provider_name TEXT,
    OUT provider_type TEXT,
    OUT options JSON)
RETURNS SETOF record
LANGUAGE C STRICT
AS 'MODULE_PATHNAME';

-- Global Tablespace Key Provider Management
CREATE FUNCTION pg_tde_add_global_key_provider(provider_type TEXT, provider_name TEXT, options JSON)
RETURNS INT
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_add_global_key_provider_file(provider_name TEXT, file_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_global_key_provider('file', provider_name,
                json_object('path' VALUE COALESCE(file_path, '')));
END;

CREATE FUNCTION pg_tde_add_global_key_provider_file(provider_name TEXT, file_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_add_global_key_provider('file', provider_name,
                json_object('path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_add_global_key_provider_vault_v2(provider_name TEXT,
                                                        vault_token TEXT,
                                                        vault_url TEXT,
                                                        vault_mount_path TEXT,
                                                        vault_ca_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_global_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
END;

CREATE FUNCTION pg_tde_add_global_key_provider_vault_v2(provider_name TEXT,
                                                        vault_token JSON,
                                                        vault_url JSON,
                                                        vault_mount_path JSON,
                                                        vault_ca_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_add_global_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_add_global_key_provider_kmip(provider_name TEXT,
                                                    kmip_host TEXT,
                                                    kmip_port INT,
                                                    kmip_ca_path TEXT,
                                                    kmip_cert_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_global_key_provider('kmip', provider_name,
                            json_object('host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
END;

CREATE FUNCTION pg_tde_add_global_key_provider_kmip(provider_name TEXT,
                                                    kmip_host JSON,
                                                    kmip_port JSON,
                                                    kmip_ca_path JSON,
                                                    kmip_cert_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_add_global_key_provider('vault-v2', provider_name,
                            json_object('host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
END;

-- Key Provider Management
CREATE FUNCTION pg_tde_change_database_key_provider(provider_type TEXT, provider_name TEXT, options JSON)
RETURNS INT
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_change_database_key_provider_file(provider_name TEXT, file_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_database_key_provider('file', provider_name,
                json_object('path' VALUE COALESCE(file_path, '')));
END;

CREATE FUNCTION pg_tde_change_database_key_provider_file(provider_name TEXT, file_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_database_key_provider('file', provider_name,
                json_object('path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_change_database_key_provider_vault_v2(provider_name TEXT,
                                                    vault_token TEXT,
                                                    vault_url TEXT,
                                                    vault_mount_path TEXT,
                                                    vault_ca_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_database_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
END;

CREATE FUNCTION pg_tde_change_database_key_provider_vault_v2(provider_name TEXT,
                                                    vault_token JSON,
                                                    vault_url JSON,
                                                    vault_mount_path JSON,
                                                    vault_ca_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_database_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_change_database_key_provider_kmip(provider_name TEXT,
                                                kmip_host TEXT,
                                                kmip_port INT,
                                                kmip_ca_path TEXT,
                                                kmip_cert_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_database_key_provider('kmip', provider_name,
                            json_object('host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
END;

CREATE FUNCTION pg_tde_change_database_key_provider_kmip(provider_name TEXT,
                                                kmip_host JSON,
                                                kmip_port JSON,
                                                kmip_ca_path JSON,
                                                kmip_cert_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_database_key_provider('kmip', provider_name,
                            json_object('host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
END;

-- Global Tablespace Key Provider Management
CREATE FUNCTION pg_tde_change_global_key_provider(provider_type TEXT, provider_name TEXT, options JSON)
RETURNS INT
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_change_global_key_provider_file(provider_name TEXT, file_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_global_key_provider('file', provider_name,
                json_object('path' VALUE COALESCE(file_path, '')));
END;

CREATE FUNCTION pg_tde_change_global_key_provider_file(provider_name TEXT, file_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_file_keyring_provider_options function.
    SELECT pg_tde_change_global_key_provider('file', provider_name,
                json_object('path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_change_global_key_provider_vault_v2(provider_name TEXT,
                                                           vault_token TEXT,
                                                           vault_url TEXT,
                                                           vault_mount_path TEXT,
                                                           vault_ca_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_global_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE COALESCE(vault_url, ''),
                            'token' VALUE COALESCE(vault_token, ''),
                            'mountPath' VALUE COALESCE(vault_mount_path, ''),
                            'caPath' VALUE COALESCE(vault_ca_path, '')));
END;

CREATE FUNCTION pg_tde_change_global_key_provider_vault_v2(provider_name TEXT,
                                                           vault_token JSON,
                                                           vault_url JSON,
                                                           vault_mount_path JSON,
                                                           vault_ca_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_vaultV2_keyring_provider_options function.
    SELECT pg_tde_change_global_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE vault_url,
                            'token' VALUE vault_token,
                            'mountPath' VALUE vault_mount_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_change_global_key_provider_kmip(provider_name TEXT,
                                                       kmip_host TEXT,
                                                       kmip_port INT,
                                                       kmip_ca_path TEXT,
                                                       kmip_cert_path TEXT)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_global_key_provider('kmip', provider_name,
                            json_object('host' VALUE COALESCE(kmip_host, ''),
                            'port' VALUE kmip_port,
                            'caPath' VALUE COALESCE(kmip_ca_path, ''),
                            'certPath' VALUE COALESCE(kmip_cert_path, '')));
END;

CREATE FUNCTION pg_tde_change_global_key_provider_kmip(provider_name TEXT,
                                                       kmip_host JSON,
                                                       kmip_port JSON,
                                                       kmip_ca_path JSON,
                                                       kmip_cert_path JSON)
RETURNS INT
LANGUAGE SQL
BEGIN ATOMIC
    -- JSON keys in the options must be matched to the keys in
    -- load_kmip_keyring_provider_options function.
    SELECT pg_tde_change_global_key_provider('vault-v2', provider_name,
                            json_object('host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path));
END;

CREATE FUNCTION pg_tde_is_encrypted(relation regclass)
RETURNS boolean
STRICT
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_set_key_using_database_key_provider(key_name TEXT, provider_name TEXT DEFAULT NULL, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_set_key_using_global_key_provider(key_name TEXT, provider_name TEXT DEFAULT NULL, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_set_server_key_using_global_key_provider(key_name TEXT, provider_name TEXT DEFAULT NULL, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_set_default_key_using_global_key_provider(key_name TEXT, provider_name TEXT DEFAULT NULL, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_tde_verify_key()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_verify_server_key()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_verify_default_key()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_key_info()
RETURNS TABLE ( key_name text,
                key_provider_name text,
                key_provider_id integer,
                key_creation_time timestamp with time zone)
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_server_key_info()
RETURNS TABLE ( key_name text,
                key_provider_name text,
                key_provider_id integer,
                key_creation_time timestamp with time zone)
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_default_key_info()
RETURNS TABLE ( key_name text,
                key_provider_name text,
                key_provider_id integer,
                key_creation_time timestamp with time zone)
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_delete_global_key_provider(provider_name TEXT)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_delete_database_key_provider(provider_name TEXT)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_version() RETURNS TEXT LANGUAGE C AS 'MODULE_PATHNAME';

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

CREATE EVENT TRIGGER pg_tde_ddl_start
ON ddl_command_start
EXECUTE FUNCTION pg_tde_ddl_command_start_capture();
ALTER EVENT TRIGGER pg_tde_ddl_start ENABLE ALWAYS;

CREATE EVENT TRIGGER pg_tde_ddl_end
ON ddl_command_end
EXECUTE FUNCTION pg_tde_ddl_command_end_capture();
ALTER EVENT TRIGGER pg_tde_ddl_end ENABLE ALWAYS;

-- Per database extension initialization
CREATE FUNCTION pg_tde_extension_initialize()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
SELECT pg_tde_extension_initialize();
DROP FUNCTION pg_tde_extension_initialize();

CREATE FUNCTION pg_tde_grant_database_key_management_to_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_set_key_using_database_key_provider(text, text, BOOLEAN) TO %I', target_role);
END;
$$;

CREATE FUNCTION pg_tde_grant_key_viewer_to_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_list_all_database_key_providers() TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_list_all_global_key_providers() TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_key_info() TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_server_key_info() TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_default_key_info() TO %I', target_role);

    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_verify_key() TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_verify_server_key() TO %I', target_role);
    EXECUTE format('GRANT EXECUTE ON FUNCTION pg_tde_verify_default_key() TO %I', target_role);
END;
$$;

CREATE FUNCTION pg_tde_revoke_database_key_management_from_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_set_key_using_database_key_provider(text, text, BOOLEAN) FROM %I', target_role);
END;
$$;

CREATE FUNCTION pg_tde_revoke_key_viewer_from_role(
    target_role TEXT)
RETURNS VOID
LANGUAGE plpgsql
SET search_path = @extschema@
AS $$
BEGIN
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_list_all_database_key_providers() FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_list_all_global_key_providers() FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_key_info() FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_server_key_info() FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_default_key_info() FROM %I', target_role);

    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_verify_key() FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_verify_server_key() FROM %I', target_role);
    EXECUTE format('REVOKE EXECUTE ON FUNCTION pg_tde_verify_default_key() FROM %I', target_role);
END;
$$;

-- Revoking all the privileges from the public role
SELECT pg_tde_revoke_database_key_management_from_role('public');
SELECT pg_tde_revoke_key_viewer_from_role('public');
