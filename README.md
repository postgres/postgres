# pg_tde

This is an `experimental` encrypted access method for PostgreSQL 16.

## Latest test release

To download the latest build of the main branch, use the `HEAD` release from [releases](https://github.com/Percona-Lab/pg_tde/releases).

Builds are available in a tar.gz format, containing only the required files, and as a deb package.
The deb package is built against the pgdg16 release, but this dependency is not yet enforced in the package.

## Documentation

Find more information about `pg_tde` in the [documentation](https://percona-lab.github.io/pg_tde/).

## Installation steps

1. Build and install the plugin with make [from source](#build-from-source), or download a [release](https://github.com/Percona-Lab/pg_tde/releases) and [install the package](#install-from-package)
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
        "provider": "file",
        "datafile": "/tmp/pgkeyring"
}
```

Currently the keyring configuration only supports the file provider, with a single datafile parameter.
This datafile is created and managed by Postgres, the only requirement is that postgres should be able to write to the specified path.

This setup is intended for developmenet, and stores the keys unencrypted in the specified data file.

## Build from source

1. To build `pg_tde` from source code, you require the following:

* On Debian and Ubuntu:
```sh
sudo apt install make gcc autoconf libjson-c-dev libcurl4-openssl-dev postgresql-server-dev-16
```

* On MacOS:
```sh
brew install make autoconf curl json-c gettext postresql@16
```

2. Install or build postgresql 16 [(see reference commit below)](#base-commit)
3. If postgres is installed in a non standard directory, set the `PG_CONFIG` environment variable to point to the `pg_config` executable

4. Clone the repository, build and install it with the following commands:  

```
git clone git://github.com/Percona-Lab/pg_tde
```

Compile and install the extension

```
cd pg_tde
./configure
make USE_PGXS=1
sudo make USE_PGXS=1 install
```

_See [Make Builds for Developers](https://github.com/Percona-Lab/pg_tde/wiki/Make-builds-for-developers) for more info on the build infrastructure._

## Install from package

1. Download the latest [release package](https://github.com/Percona-Lab/pg_tde/releases)

``` sh
wget https://github.com/Percona-Lab/pg_tde/releases/download/latest/pgtde-pgdg16.deb
```
2. Install the package

``` sh
sudo dpkg -i pgtde-pgdg16.deb
```

## Run in Docker

You can find docker images built from the current main branch on [Docker Hub](https://hub.docker.com/r/perconalab/pg_tde). Images build on top of [postgres:16](https://hub.docker.com/_/postgres) official image. To run it:
```
docker run --name pg-tde -e POSTGRES_PASSWORD=mysecretpassword -d perconalab/pg_tde
```
It builds and adds `pg_tde` extension to Postgres 16. Relevant `postgresql.conf` and `tde_conf.json` are created in `/etc/postgresql/` inside the container. This dir is exposed as volume.

See https://hub.docker.com/_/postgres on usage.

You can also build a docker image manually with:
```
docker build . -f ./docker/Dockerfile -t your-image-name
```

## Helper functions

The extension provides the following helper functions:

### pgtde_is_encrypted(tablename)

Returns `t` if the table is encrypted (uses the pg_tde access method), or `f` otherwise.

## Base commit

This is based on the heap code as of the following commit:

```
commit a81e5516fa4bc53e332cb35eefe231147c0e1749 (HEAD -> REL_16_STABLE, origin/REL_16_STABLE)
Author: Amit Kapila <akapila@postgresql.org>
Date:   Wed Sep 13 09:48:31 2023 +0530

    Fix the ALTER SUBSCRIPTION to reflect the change in run_as_owner option.
```
