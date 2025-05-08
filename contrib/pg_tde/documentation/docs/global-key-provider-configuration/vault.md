# Vault Configuration

You can configure `pg_tde` to use HashiCorp Vault as a global key provider for managing encryption keys securely.

!!! note

    This guide assumes that your Vault server is already set up and accessible. Vault configuration is outside the scope of this document, see [Vault's official documentation](https://developer.hashicorp.com/vault/docs) for more information.

## Example usage

```sql
    SELECT pg_tde_add_global_key_provider_vault_v2(
        'provider-name',
        'secret_token',
        'url',
        'mount',
        'ca_path'
    );
```

## Parameter descriptions

* `provider-name` is the name to identify this key provider
* `secret_token` is an access token with read and write access to the above mount point
* `url` is the URL of the Vault server
* `mount` is the mount point where the keyring should store the keys
* [optional] `ca_path` is the path of the CA file used for SSL verification

The following example is for testing purposes only. Use secure tokens and proper SSL validation in production environments:

```sql
    SELECT pg_tde_add_global_key_provider_vault_v2(
        'my-vault',
        'hvs.zPuyktykA...example...ewUEnIRVaKoBzs2',
        'http://vault.vault.svc.cluster.local:8200',
        'secret/data',
        NULL
    );
```

For more information on related functions, see the link below:

[Percona pg_tde function reference](../functions.md){.md-button}

## Next steps

[Global Principal Key Configuration :material-arrow-right:](set-principal-key.md){.md-button}
