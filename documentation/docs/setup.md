# Setup

Load the `pg_tde` at the start time. The extension requires additional shared memory; therefore,  add the `pg_tde` value for the `shared_preload_libraries` parameter and restart the `postgresql` instance.

1. Use the [ALTER SYSTEM](https://www.postgresql.org/docs/current/sql-altersystem.html) command from `psql` terminal to modify the `shared_preload_libraries` parameter.

    ```sql
    ALTER SYSTEM SET shared_preload_libraries = 'pg_tde';
    ```

2. Start or restart the `postgresql` instance to apply the changes.

    * On Debian and Ubuntu:    

       ```sh
       sudo systemctl restart postgresql.service
       ```
    
    * On RHEL and derivatives

       ```sh
       sudo systemctl restart postgresql-16
       ```

3. Create the extension using the [CREATE EXTENSION](https://www.postgresql.org/docs/current/sql-createextension.html) command. You must have the privileges of a superuser or a database owner to use this command. Connect to `psql` as a superuser for a database and run the following command:

    ```sql
    CREATE EXTENSION pg_tde;
    ```
    
    By default, the `pg_tde` extension is created for the currently used database. To enable data encryption in other databases, you must explicitly run the `CREATE EXTENSION` command against them. 

    !!! tip

        You can have the `pg_tde` extension automatically enabled for every newly created database. Modify the template `template1` database as follows: 

        ```
        psql -d template1 -c 'CREATE EXTENSION pg_tde;'
        ```

4. Set the location of the keyring configuration file in postgresql.conf: `pg_tde.keyringConfigFile = '/where/to/put/the/keyring.json'`
5. Create the [keyring configuration file](#keyring-configuration)
6. Start or restart the `postgresql` instance to apply the changes.

    * On Debian and Ubuntu:    

       ```sh
       sudo systemctl restart postgresql.service
       ```
    
    * On RHEL and derivatives

       ```sh
       sudo systemctl restart postgresql-16
       ```

7. You are all set to create encrypted tables. For that, specify `USING pg_tde` in the `CREATE TABLE` statement.
**For example**:
```sql
CREATE TABLE albums (
    album_id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    artist_id INTEGER,
    title TEXT NOT NULL,
    released DATE NOT NULL
) USING pg_tde;
```

## Keyring configuration

Create the keyring configuration file with the following contents:

=== "HashiCorp Vault"

     ```json
     {
             "provider": "vault-v2",
             "token": "ROOT_TOKEN",
             "url": "http://127.0.0.1:8200",
             "mountPath": "secret",
             "caPath": "<path/to/caFile>"
     }
     ```

     where:

     * `provider` is set to `vault-v2` since only the version 2 of the KV secrets engine is supported
     * `url` is the URL of the Vault server
     * `mountPath` is the mount point where the keyring should store the keys
     * `token` is an access token with read and write access to the above mount point
     * [optional] `caPath` is the path of the CA file used for SSL verification

=== "Local keyfile"

     ```json
     {
             "provider": "file",
             "datafile": "/tmp/pgkeyring"
     }
     ```     

     This keyring configuration has the file provider, with a single datafile parameter.     

     This datafile is created and managed by PostgreSQL, the only requirement is that `postgres` should be able to write to the specified path.     

     This setup is intended for development, and stores the keys unencrypted in the specified data file.
