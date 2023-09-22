# pg_tde

This is an `experimental` encrypted access method for PostgreSQL 16.

## Latest test release

To download the latest build of the main branch, use the `HEAD` release from [releases](https://github.com/Percona-Lab/postgres-tde-ext/releases).

Builds are available in a tar.gz format, containing only the required files, and as a deb package.
The deb package is built againts the pgdg16 release, but this dependency is not yet enforced in the package.

## Installation steps

1. Build and install the plugin with make [from source](#build-from-source), or download a [release](https://github.com/Percona-Lab/postgres-tde-ext/releases) and [install the package](#install-from-package)
2. `pg_tde` needs to be loaded at the start time. The extension requires additional shared memory; therefore,  add the `pg_tde` value for the `shared_preload_libraries` parameter and restart the `postgresql` instance.

Use the [ALTER SYSTEM](https://www.postgresql.org/docs/current/sql-altersystem.html) command from `psql` terminal to modify the `shared_preload_libraries` parameter.

```sql
ALTER SYSTEM SET shared_preload_libraries = 'pg_tde';
```

3. Start or restart the `postgresql` instance to apply the changes.

* On Debian and Ubuntu:

```sh
sudo systemctl restart postgresql.service
```

4. Create the extension using the [CREATE EXTENSION](https://www.postgresql.org/docs/current/sql-createextension.html) command. Using this command requires the privileges of a superuser or a database owner. Connect to `psql` as a superuser for a database and run the following command:

```sql
CREATE EXTENSION pg_tde;
```

5. Set the location of the keyring configuration file in postgresql.conf: `pg_tde.keyringConfigFile = '/where/to/put/the/keyring.json'`
6. Create the keyring configuration file [(see example keyring configuration)](#keyring-configuration)
7. Start or restart the `postgresql` instance to apply the changes.

* On Debian and Ubuntu:

```sh
sudo systemctl restart postgresql.service
```

## Keyring configuration

```json
{
        'provider': 'file',
        'datafile': '/tmp/pgkeyring',
}
```

Currently the keyring configuration only supports the file provider, with a single datafile parameter.
This datafile is created and managed by Postgres, the only requirement is that postgres should be able to write to the specified path.

This setup is intended for developmenet, and stores the keys unencrypted in the specified data file.

## Build from source

1. To build `pg_tde` from source code, you require the following on Ubuntu/Debian:

```sh
sudo apt install make gcc libjson-c-dev postgresql-server-dev-16
```

2. Install or build postgresql 16 [(see reference commit below)](#base-commit)
3. If postgres is installed in a non standard directory, set the `PG_CONFIG` environment variable to point to the `pg_config` executable

4. Clone the repository, build and install it with the following commands:  

```
git clone git://github.com/Percona-Lab/postgres-tde-ext
```

Compile and install the extension

```
cd postgres-tde-ext
make USE_PGXS=1
sudo make USE_PGXS=1 install
```

## Install from package

1. Download the latest [release package](https://github.com/Percona-Lab/postgres-tde-ext/releases)

``` sh
wget https://github.com/Percona-Lab/postgres-tde-ext/releases/download/latest/pgtde-pgdg16.deb
```
2. Install the package

``` sh
sudo dpkg -i pgtde-pgdg16.deb
```

## Base commit

This is based on the heap code as of the following commit:

```
commit a81e5516fa4bc53e332cb35eefe231147c0e1749 (HEAD -> REL_16_STABLE, origin/REL_16_STABLE)
Author: Amit Kapila <akapila@postgresql.org>
Date:   Wed Sep 13 09:48:31 2023 +0530

    Fix the ALTER SUBSCRIPTION to reflect the change in run_as_owner option.
```
