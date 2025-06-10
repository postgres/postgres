-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_tde" to load this file. \quit

-- Key Provider Management
CREATE FUNCTION pg_tde_add_database_key_provider(provider_type TEXT, provider_name TEXT, options JSON)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_add_database_key_provider(TEXT, TEXT, JSON) FROM PUBLIC;

CREATE FUNCTION pg_tde_add_database_key_provider_file(provider_name TEXT, file_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_add_database_key_provider('file', provider_name,
                json_object('path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_add_database_key_provider_vault_v2(provider_name TEXT,
                                                vault_url TEXT,
                                                vault_mount_path TEXT,
                                                vault_token_path TEXT,
                                                vault_ca_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_add_database_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE vault_url,
                            'mountPath' VALUE vault_mount_path,
                            'tokenPath' VALUE vault_token_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_add_database_key_provider_kmip(provider_name TEXT,
                                             kmip_host TEXT,
                                             kmip_port INT,
                                             kmip_cert_path TEXT,
                                             kmip_key_path TEXT,
                                             kmip_ca_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_add_database_key_provider('kmip', provider_name,
                            json_object('host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'certPath' VALUE kmip_cert_path,
                            'keyPath' VALUE kmip_key_path,
                            'caPath' VALUE kmip_ca_path));
END;

CREATE FUNCTION pg_tde_list_all_database_key_providers
    (OUT id INT,
    OUT provider_name TEXT,
    OUT provider_type TEXT,
    OUT options JSON)
RETURNS SETOF RECORD
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_list_all_database_key_providers() FROM PUBLIC;

CREATE FUNCTION pg_tde_list_all_global_key_providers
    (OUT id INT,
    OUT provider_name TEXT,
    OUT provider_type TEXT,
    OUT options JSON)
RETURNS SETOF RECORD
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_list_all_global_key_providers() FROM PUBLIC;

-- Global Tablespace Key Provider Management
CREATE FUNCTION pg_tde_add_global_key_provider(provider_type TEXT, provider_name TEXT, options JSON)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_add_global_key_provider(TEXT, TEXT, JSON) FROM PUBLIC;

CREATE FUNCTION pg_tde_add_global_key_provider_file(provider_name TEXT, file_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_add_global_key_provider('file', provider_name,
                json_object('path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_add_global_key_provider_vault_v2(provider_name TEXT,
                                                        vault_url TEXT,
                                                        vault_mount_path TEXT,
                                                        vault_token_path TEXT,
                                                        vault_ca_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_add_global_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE vault_url,
                            'mountPath' VALUE vault_mount_path,
                            'tokenPath' VALUE vault_token_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_add_global_key_provider_kmip(provider_name TEXT,
                                                    kmip_host TEXT,
                                                    kmip_port INT,
                                                    kmip_cert_path TEXT,
                                                    kmip_key_path TEXT,
                                                    kmip_ca_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_add_global_key_provider('kmip', provider_name,
                            json_object('host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'certPath' VALUE kmip_cert_path,
                            'keyPath' VALUE kmip_key_path,
                            'caPath' VALUE kmip_ca_path));
END;

-- Key Provider Management
CREATE FUNCTION pg_tde_change_database_key_provider(provider_type TEXT, provider_name TEXT, options JSON)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_change_database_key_provider(TEXT, TEXT, JSON) FROM PUBLIC;

CREATE FUNCTION pg_tde_change_database_key_provider_file(provider_name TEXT, file_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_change_database_key_provider('file', provider_name,
                json_object('path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_change_database_key_provider_vault_v2(provider_name TEXT,
                                                    vault_url TEXT,
                                                    vault_mount_path TEXT,
                                                    vault_token_path TEXT,
                                                    vault_ca_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_change_database_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE vault_url,
                            'mountPath' VALUE vault_mount_path,
                            'tokenPath' VALUE vault_token_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_change_database_key_provider_kmip(provider_name TEXT,
                                                kmip_host TEXT,
                                                kmip_port INT,
                                                kmip_cert_path TEXT,
                                                kmip_key_path TEXT,
                                                kmip_ca_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_change_database_key_provider('kmip', provider_name,
                            json_object('host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'caPath' VALUE kmip_ca_path,
                            'certPath' VALUE kmip_cert_path,
                            'keyPath' VALUE kmip_key_path));
END;

-- Global Tablespace Key Provider Management
CREATE FUNCTION pg_tde_change_global_key_provider(provider_type TEXT, provider_name TEXT, options JSON)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_change_global_key_provider(TEXT, TEXT, JSON) FROM PUBLIC;

CREATE FUNCTION pg_tde_change_global_key_provider_file(provider_name TEXT, file_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_change_global_key_provider('file', provider_name,
                json_object('path' VALUE file_path));
END;

CREATE FUNCTION pg_tde_change_global_key_provider_vault_v2(provider_name TEXT,
                                                           vault_url TEXT,
                                                           vault_mount_path TEXT,
                                                           vault_token_path TEXT,
                                                           vault_ca_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_change_global_key_provider('vault-v2', provider_name,
                            json_object('url' VALUE vault_url,
                            'mountPath' VALUE vault_mount_path,
                            'tokenPath' VALUE vault_token_path,
                            'caPath' VALUE vault_ca_path));
END;

CREATE FUNCTION pg_tde_change_global_key_provider_kmip(provider_name TEXT,
                                                       kmip_host TEXT,
                                                       kmip_port INT,
                                                       kmip_cert_path TEXT,
                                                       kmip_key_path TEXT,
                                                       kmip_ca_path TEXT)
RETURNS VOID
LANGUAGE SQL
BEGIN ATOMIC
    SELECT pg_tde_change_global_key_provider('kmip', provider_name,
                            json_object('host' VALUE kmip_host,
                            'port' VALUE kmip_port,
                            'certPath' VALUE kmip_cert_path,
                            'keyPath' VALUE kmip_key_path,
                            'caPath' VALUE kmip_ca_path));
END;

CREATE FUNCTION pg_tde_is_encrypted(relation REGCLASS)
RETURNS BOOLEAN
STRICT
LANGUAGE C
AS 'MODULE_PATHNAME';

CREATE FUNCTION pg_tde_set_key_using_database_key_provider(key_name TEXT, provider_name TEXT, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_set_key_using_database_key_provider(TEXT, TEXT, BOOLEAN) FROM PUBLIC;

CREATE FUNCTION pg_tde_set_key_using_global_key_provider(key_name TEXT, provider_name TEXT, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_set_key_using_global_key_provider(TEXT, TEXT, BOOLEAN) FROM PUBLIC;

CREATE FUNCTION pg_tde_set_server_key_using_global_key_provider(key_name TEXT, provider_name TEXT, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_set_server_key_using_global_key_provider(TEXT, TEXT, BOOLEAN) FROM PUBLIC;

CREATE FUNCTION pg_tde_set_default_key_using_global_key_provider(key_name TEXT, provider_name TEXT, ensure_new_key BOOLEAN DEFAULT FALSE)
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;
REVOKE ALL ON FUNCTION pg_tde_set_default_key_using_global_key_provider(TEXT, TEXT, BOOLEAN) FROM PUBLIC;

CREATE FUNCTION pg_tde_verify_key()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_verify_key() FROM PUBLIC;

CREATE FUNCTION pg_tde_verify_server_key()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_verify_server_key() FROM PUBLIC;

CREATE FUNCTION pg_tde_verify_default_key()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_verify_default_key() FROM PUBLIC;

CREATE FUNCTION pg_tde_delete_key()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_delete_key() FROM PUBLIC;

CREATE FUNCTION pg_tde_delete_default_key()
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_delete_default_key() FROM PUBLIC;

CREATE FUNCTION pg_tde_key_info()
RETURNS TABLE ( key_name TEXT,
                key_provider_name TEXT,
                key_provider_id INT,
                key_creation_time TIMESTAMP WITH TIME ZONE)
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_key_info() FROM PUBLIC;

CREATE FUNCTION pg_tde_server_key_info()
RETURNS TABLE ( key_name TEXT,
                key_provider_name TEXT,
                key_provider_id INT,
                key_creation_time TIMESTAMP WITH TIME ZONE)
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_server_key_info() FROM PUBLIC;

CREATE FUNCTION pg_tde_default_key_info()
RETURNS TABLE ( key_name TEXT,
                key_provider_name TEXT,
                key_provider_id INT,
                key_creation_time TIMESTAMP WITH TIME ZONE)
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_default_key_info() FROM PUBLIC;

CREATE FUNCTION pg_tde_delete_global_key_provider(provider_name TEXT)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_delete_global_key_provider(TEXT) FROM PUBLIC;

CREATE FUNCTION pg_tde_delete_database_key_provider(provider_name TEXT)
RETURNS VOID
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_delete_database_key_provider(TEXT) FROM PUBLIC;

CREATE FUNCTION pg_tde_version() RETURNS TEXT LANGUAGE C AS 'MODULE_PATHNAME';

-- Table access method
CREATE FUNCTION pg_tdeam_handler(internal)
RETURNS TABLE_AM_HANDLER
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tdeam_handler(internal) FROM PUBLIC;

CREATE ACCESS METHOD tde_heap TYPE TABLE HANDLER pg_tdeam_handler;
COMMENT ON ACCESS METHOD tde_heap IS 'tde_heap table access method';

CREATE FUNCTION pg_tde_ddl_command_start_capture()
RETURNS EVENT_TRIGGER
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_ddl_command_start_capture() FROM PUBLIC;

CREATE FUNCTION pg_tde_ddl_command_end_capture()
RETURNS EVENT_TRIGGER
LANGUAGE C
AS 'MODULE_PATHNAME';
REVOKE ALL ON FUNCTION pg_tde_ddl_command_end_capture() FROM PUBLIC;

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
