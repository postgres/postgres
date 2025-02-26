# Set up multi-tenancy

The steps below describe how to set up multi-tenancy with `pg_tde`. Multi-tenancy allows you to encrypt different databases with different keys. This provides granular control over data and enables you to introduce different security policies and access controls for each database so that only authorized users of specific databases have access to the data.

If you don't need multi-tenancy, use the global key provider. See the configuration steps from the [Setup](setup.md) section.

For how to enable WAL encryption, refer to the [WAL encryption](setup.md#wal-encryption) section.

--8<-- "kms-considerations.md"

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

3. Create the extension using the [CREATE EXTENSION](https://www.postgresql.org/docs/current/sql-createextension.html) command. You must have the privileges of a superuser or a database owner to use this command. Connect to `psql` as a superuser for a database and run the following command:

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

You must do these steps for every database where you have created the extension.

1. Set up a key provider.

    === "With KMIP server"

        Make sure you have obtained the root certificate for the KMIP server and the keypair for the client. The client key needs permissions to create / read keys on the server. Find the [configuration guidelines for the HashiCorp Vault Enterprise KMIP Secrets Engine](https://developer.hashicorp.com/vault/tutorials/enterprise/kmip-engine).
        
        For testing purposes, you can use the PyKMIP server which enables you to set up required certificates. To use a real KMIP server, make sure to obtain the valid certificates issued by the key management appliance. 

        ```
        SELECT pg_tde_add_key_provider_kmip('provider-name','kmip-addr', 5696, '/path_to/server_certificate.pem', '/path_to/client_key.pem');
        ```

        where:

        * `provider-name` is the name of the provider. You can specify any name, it's for you to identify the provider.
        * `kmip-addr` is the IP address of a domain name of the KMIP server
        * `port` is the port to communicate with the KMIP server. Typically used port is 5696.
        * `server-certificate` is the path to the certificate file for the KMIP server.
        * `client key` is the path to the client key.

        <i warning>:material-information: Warning:</i> This example is for testing purposes only:

        ```
        SELECT pg_tde_add_key_provider_kmip('kmip','127.0.0.1', 5696, '/tmp/server_certificate.pem', '/tmp/client_key_jane_doe.pem');
        ```

    === "With HashiCorp Vault"

        The Vault server setup is out of scope of this document.

        ```sql
        SELECT pg_tde_add_key_provider_vault_v2('provider-name','root_token','url','mount','ca_path');
        ``` 

        where: 

        * `url` is the URL of the Vault server
        * `mount` is the mount point where the keyring should store the keys
        * `root_token` is an access token with read and write access to the above mount point
        * [optional] `ca_path` is the path of the CA file used for SSL verification

        <i warning>:material-information: Warning:</i> This example is for testing purposes only:

	    ```
	    SELECT pg_tde_add_key_provider_file_vault_v2('my-vault','http://vault.vault.svc.cluster.local:8200,'secret/data','hvs.zPuyktykA...example...ewUEnIRVaKoBzs2', NULL);
	    ```

    === "With a keyring file"

        This setup is intended for development and stores the keys unencrypted in the specified data file.    

        ```sql
        SELECT pg_tde_add_key_provider_file('provider-name','/path/to/the/keyring/data.file');
        ```

	    <i warning>:material-information: Warning:</i> This example is for testing purposes only:

	    ```sql
	    SELECT pg_tde_add_key_provider_file('file-keyring','/tmp/pg_tde_test_local_keyring.per');
	    ```
       
       
2. Add a principal key

    ```sql
    SELECT pg_tde_set_principal_key('name-of-the-principal-key', 'provider-name','ensure_new_key');
    ```

    where:

    * `name-of-the-principal-key` is the name of the principal key. You will use this name to identify the key.
    * `provider-name` is the name of the key provider you added before. The principal key will be associated with this provider.
    * `ensure_new_key` defines if a principal key must be unique. The default value `true` means that you must speficy a unique key during key rotation. The `false` value allows reusing an existing principal key.

    <i warning>:material-information: Warning:</i> This example is for testing purposes only:

    ```sql
    SELECT pg_tde_set_principal_key('test-db-master-key','file-vault','ensure_new_key');
    ```

    The key is auto-generated.


   <i info>:material-information: Info:</i> The key provider configuration is stored in the database catalog in an unencrypted table. See [how to use external reference to parameters](external-parameters.md) to add an extra security layer to your setup.