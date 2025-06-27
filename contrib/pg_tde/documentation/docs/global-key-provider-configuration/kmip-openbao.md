# Using OpenBao as a Key Provider

You can configure `pg_tde` to use OpenBao as a global key provider for managing encryption keys securely.

!!! note
    This guide assumes that your OpenBao server is already set up and accessible. OpenBao configuration is outside the scope of this document, see [OpenBao's official documentation](https://openbao.org/docs/) for more information.

## Example usage

To register an OpenBao server as a global key provider:

```sql
SELECT pg_tde_add_global_key_provider_vault_v2(
    'provider-name',
    'url',
    'mount',
    'secret_token_path',
    'ca_path'
);
```

## Parameter descriptions

* `provider-name` is the name to identify this key provider
* `secret_token_path` is a path to the file that contains an access token with read and write access to the above mount point
* `url` is the URL of the Vault server
* `mount` is the mount point where the keyring should store the keys
* [optional] `ca_path` is the path of the CA file used for SSL verification

The following example is for testing purposes only. Use secure tokens and proper SSL validation in production environments:

```sql
SELECT pg_tde_add_global_key_provider_vault_v2(
    'my-openbao-provider',
    'https://openbao.example.com:8200',
    'secret/data',
    '/path/to/token_file',
    '/path/to/ca_cert.pem'
);
```

For more information on related functions, see the link below:

[Percona pg_tde Function Reference](../functions.md){.md-button}

## Next steps

[Global Principal Key Configuration :material-arrow-right:](set-principal-key.md){.md-button}
