# Configure pg_tde

Before you can use `pg_tde` for data encryption, you must enable the extension and configure PostgreSQL to load it at startup. This setup ensures that the necessary hooks and shared memory are available for encryption operations.

!!! note
    To learn how to configure multi-tenancy, refer to the [Configure multi-tenancy](how-to/multi-tenant-setup.md) guidelines.

The `pg_tde` extension requires additional shared memory. You need to configure PostgreSQL to preload it at startup.

## 1. Configure shared_preload_libraries

You can configure the `shared_preload_libraries` parameter in two ways:

* Add the following line to the `shared_preload_libraries` file:

    ```bash
    shared_preload_libraries = 'pg_tde'
    ```

* Use the [ALTER SYSTEM :octicons-link-external-16:](https://www.postgresql.org/docs/current/sql-altersystem.html) command. Run the following command in `psql` as a **superuser**:

    ```sql
    ALTER SYSTEM SET shared_preload_libraries = 'pg_tde';
    ```

## 2. Restart the PostgreSQL cluster

Restart the `postgresql` cluster to apply the configuration.

* On Debian and Ubuntu:

       ```sh
       sudo systemctl restart postgresql.service
       ```

* On RHEL and derivatives:

       ```sh
       sudo systemctl restart postgresql-17
       ```

## 3. Create the extension

After restarting PostgreSQL, connect to `psql` as a **superuser** or **database owner** and run:

```sql
CREATE EXTENSION pg_tde;
```

See [CREATE EXTENSION :octicons-link-external-16:](https://www.postgresql.org/docs/current/sql-createextension.html) for more details.

!!! note

    The `pg_tde` extension is created only for the current database. To enable it for other databases, you must run the command in each individual database.

## 4. (Optional) Enable pg_tde by default

To automatically have `pg_tde` enabled for all new databases, modify the `template1` database:

```
psql -d template1 -c 'CREATE EXTENSION pg_tde;'
```

!!! note

    You can use external key providers to manage encryption keys. The recommended approach is to use the Key Management Store (KMS). See the next step on how to configure the KMS.

## Next steps

[Configure Key Management (KMS) :material-arrow-right:](global-key-provider-configuration/index.md){.md-button}
