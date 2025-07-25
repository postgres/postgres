# Configure WAL Encryption (tech preview)

!!! warning
    The WAL encryption feature is currently in beta and is not effective unless explicitly enabled. It is not yet production ready. **Do not enable this feature in production environments**.

Before enabling WAL encryption, follow the steps below to create a principal key and configure it for WAL:

1. Create the `pg_tde` extension if it does not exist:

    ```sql
    CREATE EXTENSION IF NOT EXISTS pg_tde;
    ```

2. Set up the key provider for WAL encryption:

    === "With KMIP server"

        Make sure you have obtained the root certificate for the KMIP server and the keypair for the client. The client key needs permissions to create / read keys on the server. Find the [configuration guidelines for the HashiCorp Vault Enterprise KMIP Secrets Engine](https://developer.hashicorp.com/vault/tutorials/enterprise/kmip-engine).

        For testing purposes, you can use the PyKMIP server which enables you to set up required certificates. To use a real KMIP server, make sure to obtain the valid certificates issued by the key management appliance.

        ```sql
        SELECT pg_tde_add_global_key_provider_kmip(
            'provider-name', 
            'kmip-addr', 
            5696, 
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
        SELECT pg_tde_add_key_using_global_key_provider_kmip(
            'kmip', 
            '127.0.0.1', 
            5696, 
            '/tmp/client_cert_jane_doe.pem', 
            '/tmp/client_key_jane_doe.pem', 
            '/tmp/server_certificate.pem'
        );
        ```

    === "With HashiCorp Vault"

        ```sql
        SELECT pg_tde_add_global_key_provider_vault_v2(
            'provider-name', 
            'url', 
            'mount', 
            'secret_token_path', 
            'ca_path'
        );
        ```

        where:

        * `provider-name` is the name you define for the key provider
        * `url` is the URL of the Vault server
        * `mount` is the mount point where the keyring should store the keys
        * `secret_token_path` is a path to the file that contains an access token with read and write access to the above mount point
        * [optional] `ca_path` is the path of the CA file used for SSL verification

    === "With keyring file"

        This setup is **not recommended**, as it is intended for development.
        
        <i warning>:material-information: Warning:</i> The keys are stored **unencrypted** in the specified data file.

        ```sql
        SELECT pg_tde_add_global_key_provider_file(
            'provider-name', 
            '/path/to/the/keyring/data.file'
        );
        ```

3. Create principal key

    ```sql
    SELECT pg_tde_set_server_key_using_global_key_provider(
        'key', 
        'provider-name'
    );
    ```

4. Enable WAL level encryption using the `ALTER SYSTEM` command. You need the privileges of the superuser to run this command:

    ```sql
    ALTER SYSTEM SET pg_tde.wal_encrypt = on;
    ```

5. Restart the server to apply the changes.

    * On Debian and Ubuntu:

    ```sh
    sudo systemctl restart postgresql
    ```

    * On RHEL and derivatives

    ```sh
    sudo systemctl restart postgresql-17
    ```

Now WAL files start to be encrypted for both encrypted and unencrypted tables.

For more technical references related to architecture, variables or functions, see:
[Technical Reference](advanced-topics/tech-reference.md){.md-button}
