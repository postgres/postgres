# WAL encryption configuration (tech preview)

After you [enabled `pg_tde`](#enable-extension) and restarted the Percona Server for PostgreSQL, a principal key and a keyring for WAL are created. Now you need to instruct `pg_tde ` to encrypt WAL files by configuring WAL encryption. 

Here's how to do it:

1. Enable WAL level encryption using the `ALTER SYSTEM SET` command. You need the superuser privileges to run this command:

    ```sql
    ALTER SYSTEM set pg_tde.wal_encrypt = on;
    ```

2. Restart the server to apply the changes.

    * On Debian and Ubuntu:    

       ```sh
       sudo systemctl restart postgresql-17
       ```
    
    * On RHEL and derivatives

       ```sh
       sudo systemctl restart postgresql-17
       ```

3. We highly recommend you to create your own keyring and rotate the principal key. This is because the default principal key is created from the local keyfile and is stored unencrypted. 

    Set up the key provider for WAL encryption

    === "With KMIP server"

        Make sure you have obtained the root certificate for the KMIP server and the keypair for the client. The client key needs permissions to create / read keys on the server. Find the [configuration guidelines for the HashiCorp Vault Enterprise KMIP Secrets Engine](https://developer.hashicorp.com/vault/tutorials/enterprise/kmip-engine).
        
        For testing purposes, you can use the PyKMIP server which enables you to set up required certificates. To use a real KMIP server, make sure to obtain the valid certificates issued by the key management appliance. 

        ```sql
        SELECT pg_tde_add_global_key_provider_kmip('provider-name','kmip-addr', 5696, '/path_to/server_certificate.pem', '/path_to/client_key.pem');
        ```

        where:

        * `provider-name` is the name of the provider. You can specify any name, it's for you to identify the provider.
        * `kmip-addr` is the IP address of a domain name of the KMIP server
        * `port` is the port to communicate with the KMIP server. Typically used port is 5696.
        * `server-certificate` is the path to the certificate file for the KMIP server.
        * `client key` is the path to the client key.

        <i warning>:material-information: Warning:</i> This example is for testing purposes only:

        ```
        SELECT pg_tde_add_key_global_provider_kmip('kmip','127.0.0.1', 5696, '/tmp/server_certificate.pem', '/tmp/client_key_jane_doe.pem');
        ```
    
    === "With HashiCorp Vault"
    
        ```sql
        SELECT pg_tde_add_global_key_provider_vault_v2('provider-name',:'secret_token','url','mount','ca_path');
        ``` 

        where: 

        * `provider-name` is the name you define for the key provider
        * `url` is the URL of the Vault server
        * `mount` is the mount point where the keyring should store the keys
        * `secret_token` is an access token with read and write access to the above mount point
        * [optional] `ca_path` is the path of the CA file used for SSL verification


    === "With keyring file"

        This setup is intended for development and stores the keys unencrypted in the specified data file.    

        ```sql
        SELECT pg_tde_add_key_provider_file('provider-name','/path/to/the/keyring/data.file');
        ```

4. Rotate the principal key for WAL encryption. 

    ```sql
    SELECT pg_tde_set_server_principal_key('new-principal-key', 'provider-name','ensure_new_key');
    ```

    The `ensure_new_key` parameter is set to `true` by default. It ensures that a new key must be unique. If set to `false`, an existing principal key will be reused.  


Now all WAL files are encrypted for both encrypted and unencrypted tables.

## Next steps

[Test TDE](test.md){.md-button}