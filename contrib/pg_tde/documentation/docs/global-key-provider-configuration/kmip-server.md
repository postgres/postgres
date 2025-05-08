# KMIP Configuration

To use a Key Management Interoperability Protocol (KMIP) server with `pg_tde`, you must configure it as a global key provider. This setup enables `pg_tde` to securely fetch and manage encryption keys from a centralized key management appliance.

!!! note

    You need the root certificate of the KMIP server and a client key/certificate pair with permissions to create and read keys on the server.

It is recommended to review the [configuration guidelines for the HashiCorp Vault Enterprise KMIP Secrets Engine](https://developer.hashicorp.com/vault/tutorials/enterprise/kmip-engine) if you're using Vault.

For testing purposes, you can use a lightweight PyKMIP server, which enables easy certificate generation and basic KMIP behavior. If you're using a production-grade KMIP server, ensure you obtain valid, trusted certificates from the key management appliance.

## Example usage

    ```sql
    SELECT pg_tde_add_global_key_provider_kmip(
        'provider-name',
        'kmip-IP', 
        5696,
        '/path_to/server_certificate.pem', 
        '/path_to/client_cert.pem',
        '/path_to/client_key.pem'
    );
    ```

## Parameter descriptions

* `provider-name` is the name of the provider. You can specify any name, it's for you to identify the provider
* `kmip-IP` is the IP address of a domain name of the KMIP server
* `port` is the port to communicate with the KMIP server. Typically used port is 5696
* `server-certificate` is the path to the certificate file for the KMIP server
* `client_cert` is the path to the client certificate.
* `client_key` (optional) is the path to the client key. If not specified, the certificate key has to contain both the certifcate and the key.

<i warning>:material-information: Warning:</i> `pg_tde_add_global_key_provider_kmip` currently accepts only a combined client key and a client certificate for its final parameter, reffered to as `client key`.

The following example is for testing purposes only.

    ```sql
    SELECT pg_tde_add_global_key_provider_kmip(
        'kmip','127.0.0.1', 
        5696, 
        '/tmp/server_certificate.pem', 
        '/tmp/client_cert_jane_doe.pem',
        '/tmp/client_key_jane_doe.pem'
    );
    ```

For more information on related functions, see the link below:

[Percona pg_tde function reference](../functions.md){.md-button}

## Next steps

[Global Principal Key Configuration :material-arrow-right:](set-principal-key.md){.md-button}
