# Global Principal Key Configuration

You can configure a default principal key using a global key provider. This key will be used by all databases that do not have their own encryption keys configured.

## Create a default principal key

Run the following command:

```sql
SELECT pg_tde_set_default_key_using_global_key_provider(
    'name-of-the-key',
    'provider-name',
    'ensure_new_key'
);
```

## Parameter description

* `name-of-the-key` is the name of the principal key. You will use this name to identify the key.
* `provider-name` is the name of the key provider you added before. The principal key will be associated with this provider.
* `ensure_new_key` defines if a principal key must be unique. The default value `true` means that you must speficy a unique key during key rotation. The `false` value allows reusing an existing principal key.

This example is for testing purposes only. Replace the key name and provider name with your values:

```sql
SELECT pg_tde_set_key_using_global_key_provider(
    'test-db-master-key',
    'file-vault',
    'ensure_new_key'
);
```

!!! note
    The key is auto-generated.

After this, all databases that do not have something else configured will use this newly generated principal key.

## Next steps

[Validate Encryption with pg_tde :material-arrow-right:](../test.md){.md-button}
