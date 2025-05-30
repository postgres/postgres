# Keyring File Configuration

This setup is intended for development and stores the keys unencrypted in the specified data file.

!!! note
     While keyfiles may be acceptable for **local** or **testing environments**, KMS integration is the recommended approach for production deployments.
  
```sql
SELECT pg_tde_add_global_key_provider_file(
    'provider-name',
    '/path/to/the/keyring/data.file'
);
```

The following example is used for testing purposes only:

```sql
SELECT pg_tde_add_global_key_provider_file(
    'file-keyring',
    '/tmp/pg_tde_test_local_keyring.per'
);
```

## Next steps

[Global Principal Key Configuration :material-arrow-right:](set-principal-key.md){.md-button}
