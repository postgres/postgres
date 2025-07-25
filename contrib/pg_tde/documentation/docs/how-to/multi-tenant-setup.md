# Configure Multi-tenancy

The steps below describe how to set up multi-tenancy with `pg_tde`. Multi-tenancy allows you to encrypt different databases with different keys. This provides granular control over data and enables you to introduce different security policies and access controls for each database so that only authorized users of specific databases have access to the data.

If you don't need multi-tenancy, use the global key provider. See the configuration steps from the [Configure pg_tde :octicons-link-external-16:](../setup.md) section.

For how to enable WAL encryption, refer to the [Configure WAL Encryption :octicons-link-external-16:](../wal-encryption.md) section.

--8<-- "kms-considerations.md"

!!! note
    If no error is reported when running the commands below, the operation completed successfully.

## Enable extension

Load the `pg_tde` at startup time. The extension requires additional shared memory; therefore, add the `pg_tde` value for the `shared_preload_libraries` parameter and restart the `postgresql` cluster.

1. Use the [ALTER SYSTEM :octicons-link-external-16:](https://www.postgresql.org/docs/current/sql-altersystem.html) command from `psql` terminal to modify the `shared_preload_libraries` parameter. This requires superuser privileges.

    ```
    ALTER SYSTEM SET shared_preload_libraries = 'pg_tde';
    ```

2. Start or restart the `postgresql` cluster to apply the changes.

    * On Debian and Ubuntu:

       ```sh
       sudo systemctl restart postgresql-17
       ```

    * On RHEL and derivatives

       ```sh
       sudo systemctl restart postgresql-17
       ```

3. Create the extension using the [CREATE EXTENSION :octicons-link-external-16:](https://www.postgresql.org/docs/current/sql-createextension.html) command. You must have the privileges of a superuser or a database owner to use this command. Connect to `psql` as a superuser for a database and run the following command:

    ```
    CREATE EXTENSION pg_tde;
    ```

    The `pg_tde` extension is created for the currently used database. To enable data encryption in other databases, you must explicitly run the `CREATE EXTENSION` command against them.

    !!! tip

        You can have the `pg_tde` extension automatically enabled for every newly created database. Modify the template `template1` database as follows:

        ```sh
        psql -d template1 -c 'CREATE EXTENSION pg_tde;'
        ```

## Key provider configuration

You must do these steps for every database where you have created the extension. For more information on configurations, please see the [Configure Key Management (KMS) :octicons-link-external-16:](../global-key-provider-configuration/overview.md) topic.

1. Set up a key provider.

    === "With KMIP server"

        The KMIP server setup is out of scope of this document. 

        Make sure you have obtained the root certificate for the KMIP server and the keypair for the client. The client key needs permissions to create / read keys on the server. Find the [configuration guidelines for the HashiCorp Vault Enterprise KMIP Secrets Engine :octicons-link-external-16:](https://developer.hashicorp.com/vault/tutorials/enterprise/kmip-engine).

        For testing purposes, you can use the PyKMIP server which enables you to set up required certificates. To use a real KMIP server, make sure to obtain the valid certificates issued by the key management appliance.

        ```sql
        SELECT pg_tde_add_database_key_provider_kmip(
          'provider-name',
          'kmip-addr', 
          `port`, 
          '/path_to/client_cert.pem', 
          '/path_to/client_key.pem', 
          '/path_to/server_certificate.pem'
          );
        ```

        where:

        * `provider-name` is the name of the provider. You can specify any name, it's for you to identify the provider.
        * `kmip-addr` is the IP address of a domain name of the KMIP server
        * `port` is the port to communicate with the KMIP server. Typically used port is 5696.
        * `server-certificate` is the path to the certificate file for the KMIP server.
        * `client-cert` is the path to the client certificate.
        * `client-key` is the path to the client key.

        <i warning>:material-information: Warning:</i> This example is for testing purposes only:

        ```sql
        SELECT pg_tde_add_database_key_provider_kmip(
            'kmip', 
            '127.0.0.1', 
            5696, 
            '/tmp/client_cert_jane_doe.pem', 
            '/tmp/client_key_jane_doe.pem', 
            '/tmp/server_certificate.pem'
            );
        ```

    === "With HashiCorp Vault"

        The Vault server setup is out of scope of this document.

        ```sql
        SELECT pg_tde_add_database_key_provider_vault_v2(
            'provider-name', 
            'url', 
            'mount', 
            'secret_token_path', 
            'ca_path'
            );
        ```

        where:

        * `url` is the URL of the Vault server
        * `mount` is the mount point where the keyring should store the keys
        * `secret_token_path` is a path to the file that contains an access token with read and write access to the above mount point
        * [optional] `ca_path` is the path of the CA file used for SSL verification

        <i warning>:material-information: Warning:</i> This example is for testing purposes only:

	    ```sql
	    SELECT pg_tde_add_database_key_provider_file_vault_v2(
            'my-vault',
            'http://vault.vault.svc.cluster.local:8200',
            'secret/data',
            'hvs.zPuyktykA...example...ewUEnIRVaKoBzs2', 
            NULL
            );
	    ```

    === "With a keyring file (not recommended)"

        This setup is intended for development and stores the keys unencrypted in the specified data file.

        ```sql
        SELECT pg_tde_add_database_key_provider_file(
            'provider-name', 
            '/path/to/the/keyring/data.file'
            );
        ```

	    <i warning>:material-information: Warning:</i> This example is for testing purposes only:

	    ```sql
	    SELECT pg_tde_add_database_key_provider_file(
            'file-keyring', 
            '/tmp/pg_tde_test_local_keyring.per'
            );
	    ```

2. Create a key
    ```sql

    SELECT pg_tde_create_key_using_database_key_provider(
        'name-of-the-key', 
        'provider-name'
        );
    ```

    where:

    * `name-of-the-key` is the name of the principal key. You will use this name to identify the key.
    * `provider-name` is the name of the key provider you added before. The principal key is associated with this provider and it is the location where it is stored and fetched from.

    <i warning>:material-information: Warning:</i> This example is for testing purposes only:

    ```sql
    SELECT pg_tde_create_key_using_database_key_provider(
        'test-db-master-key', 
        'file-vault'
        );
    ```

    !!! note
        The key is auto-generated.

3. Use the key as principal key
    ```sql

    SELECT pg_tde_set_key_using_database_key_provider(
        'name-of-the-key', 
        'provider-name'
        );
    ```

    where:

    * `name-of-the-key` is the name of the principal key. You will use this name to identify the key.
    * `provider-name` is the name of the key provider you added before. The principal key will be associated with this provider.

    <i warning>:material-information: Warning:</i> This example is for testing purposes only:

    ```sql
    SELECT pg_tde_set_key_using_database_key_provider(
        'test-db-master-key',
        'file-vault'
        );
    ```
