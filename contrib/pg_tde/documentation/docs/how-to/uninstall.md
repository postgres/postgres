# Uninstall pg_tde

If you no longer wish to use Transparent Data Encryption (TDE) in your deployment, you can remove the `pg_tde` extension.

To proceed, you must have one of the following privileges:

- Superuser privileges (to remove the extension globally), or
- Database owner privileges (to remove it from a specific database only)

To uninstall `pg_tde`, follow the steps below.

## Step 1. Remove `pg_tde` from all databases

Before uninstalling, you must remove the extension from every database where it is loaded. This includes template databases if `pg_tde` was previously enabled there.

a. Clean up encrypted tables:

To decrypt a table and restore it to its default storage method:

```sql
ALTER TABLE <table_name> SET ACCESS METHOD heap;
```

b. Remove the extension once all encrypted tables have been handled:

```sql
DROP EXTENSION pg_tde;
```

!!! note

    If there are any encrypted objects that were not previously decrypted or deleted, this command will fail and you have to follow the steps above for these objects.

## Step 2. Turn off WAL encryption

If you are using WAL encryption, you need to **turn it off** before you uninstall the `pg_tde` library:

a. Run:

```sql
ALTER SYSTEM SET pg_tde.wal_encrypt = off;
```

b. Restart the PostgreSQL cluster to apply the changes:

- On Debian and Ubuntu:

```sh
sudo systemctl restart postgresql
```

- On RHEL and derivatives:

```sh
sudo systemctl restart postgresql-17
```

## Step 3. Uninstall the `pg_tde` shared library

!!! warning

    This process removes the extension, but **does not** decrypt data automatically. Only uninstall the shared library after all encrypted data **has been removed or decrypted** and WAL encryption **has been disabled**.

!!! note

    Encrypted WAL pages **will not be decrypted**, so any postgres cluster needing to read them will need the `pg_tde` library loaded, and the WAL encryption keys available and in use.

At this point, the shared library is still loaded but no longer active. To fully uninstall `pg_tde`, complete the steps below.

a. Run `SHOW shared_preload_libraries` to view the current configuration of preloaded libraries.

For example:

```sql
postgres=# SHOW shared_preload_libraries;
        shared_preload_libraries
-----------------------------------------
pg_stat_statements,pg_tde,auto_explain
(1 row)

postgres=#
```

b. Remove `pg_tde` from the list and apply the new setting using `ALTER SYSTEM SET shared_preload_libraries=<your list of libraries>`.

For example:

```sql
postgres=# ALTER SYSTEM SET shared_preload_libraries=pg_stat_statements,auto_explain;
ALTER SYSTEM
postgres=#
```

!!! note

    Your list of libraries will most likely be different than the above example.
    
    If `pg_tde` is the only shared library in the list, and it was set via `postgresql.conf` you cannot disable it using the `ALTER SYSTEM SET ...` command. Instead:
    
    1. Remove the `shared_preload_libraries` line from `postgresql.conf`
    2. Run `ALTER SYSTEM RESET shared_preload_libraries;`

c. Restart the `postgresql` cluster to apply the changes:

- On Debian and Ubuntu:

    ```sh
    sudo systemctl restart postgresql
    ```

- On RHEL and derivatives:

    ```sh
    sudo systemctl restart postgresql-17
    ```

## Step 4. (Optional) Clean up configuration

At this point it is safe to remove any configuration related to `pg_tde` from `postgresql.conf` and `postgresql.auto.conf`. Look for any configuration parameters prefixed with `pg_tde.` and remove or comment them out, as needed.

## Troubleshooting: PANIC checkpoint not found on restart

This can happen if WAL encryption was not properly disabled before removing `pg_tde` from `shared_preload_libraries`, when the PostgreSQL server was not restarted after disabling WAL encryption (see step 3.c).

You might see this when restarting the PostgreSQL cluster:

```
2025-04-01 17:12:50.607 CEST [496385] PANIC:  could not locate a valid checkpoint record at 0/17B2580
```

To resolve it follow these steps:

1. Re-add `pg_tde` to `shared_preload_libraries`
2. Restart the PostgreSQL cluster
3. Follow the [instructions for turning off WAL encryption](#step-2-turn-off-wal-encryption) before uninstalling the shared library again

!!! note

    Two restarts are required to uninstall properly if WAL encryption was enabled:
    
    - First to disable WAL encryption
    - Second to remove the `pg_tde` library
