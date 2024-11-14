[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/percona/pg_tde/badge)](https://scorecard.dev/viewer/?uri=github.com/percona/pg_tde)
[![Forum](https://img.shields.io/badge/Forum-join-brightgreen)](https://forums.percona.com/)

# pg_tde: Transparent Database Encryption for PostgreSQL

The PostgreSQL extension provides data at rest encryption. It is currently in an experimental phase and is under active development. [We need your feedback!](https://github.com/percona/pg_tde/discussions/151)

## Table of contents
1. [Overview](#overview)
2. [Documentation](#documentation)
1. [Percona Server for PostgreSQL](#percona-server-for-postgresql)
3. [Build from sources](#building-from-sources-for-community-postgresql)
4. [Run in docker](#run-in-docker)
5. [Setting up](#setting-up)
6. [Helper functions](#helper-functions)

## Overview
Transparent Data Encryption offers encryption at the file level and solves the problem of protecting data at rest. The encryption is transparent for users allowing them to access and manipulate the data and not to worry about the encryption process. As a key provider, the extension supports the keyringfile and  [Hashicorp Vault](https://www.vaultproject.io/).

### This extension provides two `access methods` with different options:

#### `tde_heap_basic` access method
- Works with community PostgreSQL 16 and 17 or with [Percona Server for PosgreSQL 17](https://docs.percona.com/postgresql/17/postgresql-server.html)
- Encrypts tuples and WAL
- **Doesn't** encrypt indexes, temporary files, statistics
- CPU expensive as it decrypts pages each time they are read from bufferpool

#### `tde_heap` access method
- Works only with [Percona Server for PostgreSQL 17](https://docs.percona.com/postgresql/17/postgresql-server.html)
- Uses extended Storage Manager and WAL APIs
- Encrypts tuples, WAL and indexes
- **Doesn't** encrypt temporary files and statistics **yet**
- Faster and cheaper than `tde_heap_basic`

## Documentation

Full and comprehensive documentation about `pg_tde` is available at https://percona.github.io/pg_tde/.

## Percona Server for PostgreSQL

Percona provides binary packages of `pg_tde` extension only for Percona Server for PostgreSQL. Learn how to install them or build `pg_tde` from sources for PSPG in the [documentation](https://percona.github.io/pg_tde/main/install.html).

## Building from sources for community PostgreSQL
  1. Install required dependencies (replace XX with 16 or 17)
   - On Debian and Ubuntu:
        ```sh
        sudo apt install make gcc autoconf git libcurl4-openssl-dev postgresql-server-dev-XX
        ```
     
   - On RHEL 8 compatible OS:
        ```sh
        sudo yum install epel-release
        yum --enablerepo=powertools install git make gcc autoconf libcurl-devel perl-IPC-Run redhat-rpm-config openssl-devel postgresqlXX-devel
        ```

   - On MacOS:
        ```sh
        brew install make autoconf curl gettext postresql@XX
        ```

  2. Install or build postgresql 16 or 17
  3. If postgres is installed in a non standard directory, set the `PG_CONFIG` environment variable to point to the `pg_config` executable

  4. Clone the repository, build and install it with the following commands:  

     ```sh
     git clone https://github.com/percona/pg_tde
     ```
  
   5. Compile and install the extension

      ```sh
      cd pg_tde
      ./configure
      make USE_PGXS=1
      sudo make USE_PGXS=1 install
      ```

## Run in Docker

There is a [docker image](https://hub.docker.com/r/perconalab/pg_tde) with `pg_tde` based community [PostgreSQL 16](https://hub.docker.com/_/postgres) 

```
docker run --name pg-tde -e POSTGRES_PASSWORD=mysecretpassword -d perconalab/pg_tde
```
Docker file is available [here](https://github.com/percona/pg_tde/blob/main/docker/Dockerfile)


_See [Make Builds for Developers](https://github.com/percona/pg_tde/wiki/Make-builds-for-developers) for more info on the build infrastructure._

## Setting up

  1. Add extension to the `shared_preload_libraries`:
      1. Via configuration file `postgresql.conf `
            ```
            shared_preload_libraries=pg_tde 
            ```
      2. Via SQL using [ALTER SYSTEM](https://www.postgresql.org/docs/current/sql-altersystem.html) command
            ```sql
            ALTER SYSTEM SET shared_preload_libraries = 'pg_tde';
            ```
   2. Start or restart the `postgresql` instance to apply the changes.
      * On Debian and Ubuntu:

        ```sh
        sudo systemctl restart postgresql.service
        ```

      * On RHEL 8 compatible OS (replace XX with your version):
        ```sh
        sudo systemctl restart postgresql-XX.service
        ``` 
   3. [CREATE EXTENSION](https://www.postgresql.org/docs/current/sql-createextension.html) with SQL (requires superuser or a database owner privileges):

        ```sql
        CREATE EXTENSION pg_tde;
        ```
   4. Create a key provider. Currently `pg_tde` supports `File` and `Vault-V2` key providers. You can add the required key provider using one of the functions.
   

        ```sql
        -- For Vault-V2 key provider
        -- pg_tde_add_key_provider_vault_v2(provider_name, vault_token, vault_url, vault_mount_path, vault_ca_path)
        SELECT pg_tde_add_key_provider_vault_v2(
            'vault-provider',
            json_object( 'type' VALUE 'remote', 'url' VALUE 'http://localhost:8888/token' ),
            json_object( 'type' VALUE 'remote', 'url' VALUE 'http://localhost:8888/url' ),
            to_json('secret'::text), NULL);

        -- For File key provider
        -- pg_tde_add_key_provider_file(provider_name, file_path);
        SELECT pg_tde_add_key_provider_file('file','/tmp/pgkeyring');
        ```

        **Note: The `File` provided is intended for development and stores the keys unencrypted in the specified data file.**

   5. Set the principal key for the database using the `pg_tde_set_principal_key` function.

        ```sql
        -- pg_tde_set_principal_key(principal_key_name, provider_name);
        SELECT pg_tde_set_principal_key('my-principal-key','file');
        ```
   
   6. Specify `tde_heap_basic` access method during table creation
        ```sql
        CREATE TABLE albums (
            album_id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            artist_id INTEGER,
            title TEXT NOT NULL,
            released DATE NOT NULL
        ) USING tde_heap_basic;
        ```
   7. You can encrypt existing table. It requires rewriting the table, so for large tables, it might take a considerable amount of time. 
        ```sql
        ALTER TABLE table_name SET access method  tde_heap_basic;
        ```


## Latest test release

To download the latest build of the main branch, use the `HEAD` release from [releases](https://github.com/percona/pg_tde/releases).

Builds are available in a tar.gz format, containing only the required files, and as a deb package.
The deb package is built against the pgdg16 release, but this dependency is not yet enforced in the package.


## Helper functions

The extension provides the following helper functions:

### pg_tde_is_encrypted(tablename)

Returns `t` if the table is encrypted (uses the tde_heap_basic access method), or `f` otherwise.
