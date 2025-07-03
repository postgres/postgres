# Streaming Replication with tde_heap

This section outlines how to set up PostgreSQL streaming replication when the `pg_tde` extension, specifically the [`tde_heap`](index/table-access-method.md) access method, is enabled on the primary server.

Before you begin, ensure you have followed the [`pg_tde` setup instructions](setup.md).

!!! note
    You do **not** need to run `CREATE EXTENSION` on the standby. It will be replicated automatically.

## 1. Configure the Primary

### Create a principal key

Use the [`pg_tde_set_server_key_using_global_key_provider`](functions.md#pg_tde_set_server_key_using_global_key_provider) function to create a principal key.

### Create the replication role

Create a replication role on the primary:

```sql
CREATE ROLE example_replicator WITH REPLICATION LOGIN PASSWORD 'example_password';
```

### Configure pg_hba.conf

To allow the replica to connect to the primary server, add the following line in `pg_hba.conf`:

```conf
host  replication  example_replicator  standby_ip/32  scram-sha-256
```

Ensure that it is placed before the other host rules for replication and then **reload** the configuration:

```sql
SELECT pg_reload_conf();
```

## 2. Configure the Standby

### Perform a database backup

Run the base backup from your standby machine to pull the encrypted base backup:

```bash
export PGPASSWORD='example_password'
pg_basebackup \
  -h primary_ip \
  -D /var/lib/pgsql/data \
  -U example_replicator \
  --wal-method=stream \
  --slot=tde_slot \
  -C \
  -c fast \
  -v -P
```

### Configure postgresql.conf

After the base backup completes, add the following line to the standby's `postgresql.conf` file:

```ini
shared_preload_libraries = 'pg_tde'
```

## 3. Start and validate replication

Assuming that the primary and the standby are running on separate hosts, start the PostgreSQL service:

```bash
sudo systemctl start postgresql
```

!!! warning "Key management consistency **required** for replication"

    If you're using a KMS provider, such as Vault or KMIP, make sure that both the primary and the standby have access to the **same** key management configuration, and that the paths to the configuration files are identical on both systems.

    For example:

    - If you configure Vault with a secret path: `/path/to/secret.file`, then that file **must** exist at the same path on both the primary and the standby.
    - If you use the `keyring_file` provider, be aware that it stores key material in a local file and it is **not designed** for shared or concurrent use across multiple servers. It is **not recommended** in replication setups.

* On primary:

```sql
SELECT client_addr, state 
FROM pg_stat_replication;
```

* On standby:

```sql
SELECT
    pg_is_in_recovery()          AS in_recovery,
    pg_last_wal_receive_lsn()    AS receive_lsn,
    pg_last_wal_replay_lsn()     AS replay_lsn;
```

!!! tip
    Want to verify that everything is working? After creating an encrypted table on the primary, run the following command on the standby to confirm that the encryption is active and the keys are resolved:

    ```sql
    SELECT pg_tde_is_encrypted('your_encrypted_table');
    ```
