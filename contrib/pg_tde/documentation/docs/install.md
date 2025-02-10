# Installation

Install `pg_tde` using one of available installation methods:


=== "Package manager" 

    The packages are available for the following operating systems:
    
    - Red Hat Enterprise Linux 8 and compatible derivatives
    - Red Hat Enterprise Linux 9 and compatible derivatives
    - Ubuntu 20.04 (Focal Fossa)
    - Ubuntu 22.04 (Jammy Jellyfish)
    - Ubuntu 24.04 (Noble Numbat)
    - Debian 11 (Bullseye) 
    - Debian 12 (Bookworm)

    [Install on Debian or Ubuntu](apt.md){.md-button}
    [Install on RHEL or derivatives](yum.md){.md-button}

=== "Build from source"

    To build `pg_tde` from source code, do the following:

    1. On Ubuntu/Debian: Install the following dependencies required for the build:

        ```sh
        sudo apt install make gcc postgresql-server-dev-17 libcurl4-openssl-dev
        ```

    2. [Install Percona Distribution for PostgreSQL 17 :octicons-link-external-16:](https://docs.percona.com/postgresql/17/installing.html) or [upstream PostgreSQL 17 :octicons-link-external-16:](https://www.postgresql.org/download/)

    3. If PostgreSQL is installed in a non standard directory, set the `PG_CONFIG` environment variable to point to the `pg_config` executable.

    4. Clone the repository:  

        ```
        git clone git://github.com/percona/pg_tde
        ```

    5. Compile and install the extension

        ```
        cd pg_tde
        make USE_PGXS=1
        sudo make USE_PGXS=1 install
        ```

=== "Run in Docker"

    !!! note

        The steps below are for the deprecated PostgreSQL Community version of `pg_tde` and apply only to PostgreSQL 16. This `pg_tde` version provides only the `tde_heap_basic` access method for data encryption. 

        To enjoy full encryption features, run the `pg_tde` version for Percona Server for PostgreSQL. [Use the Percona Distribution for PostgreSQL Docker image :octicons-link-external-16:](https://docs.percona.com/postgresql/17/docker.html). 

    You can find Docker images on [Docker Hub](https://hub.docker.com/r/perconalab/pg_tde). Images are built on top of [postgres:16](https://hub.docker.com/_/postgres) official image.     

    To run `pg_tde` in Docker, use the following command:    

    ```
    docker run --name pg-tde -e POSTGRES_PASSWORD=mysecretpassword -d perconalab/pg_tde
    ```    

    It builds and adds the `pg_tde` extension to PostgreSQL 16. The `postgresql.conf` contains the required modifications. The `pg_tde` extension is added to `template1` so that all new databases automatically have the `pg_tde` extension loaded. 

    Keys are not created automatically. You must configure a key provider and a principal key for each database  where you wish to use encrypted tables. 

    Connect to the container and establish the `psql` session there. Then, see the instructions in the [Setup](setup.md) section, starting with the 4th point, as the first 3 steps are already completed in the Docker image.

    See [Docker Docs](https://hub.docker.com/_/postgres) on usage.    

    You can also build a Docker image manually with:    

    ```
    docker build . -f ./docker/Dockerfile -t your-image-name
    ```

## Next steps

[Setup](setup.md){.md-button}
