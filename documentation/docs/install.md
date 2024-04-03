# Installation

## Considerations

You can use the following options to manage encryption keys:

* Use the HashiCorp Vault server. This is the recommended approach. The Vault server configuration is out of scope of this document. We assume that you have the Vault server up and running. For the  `pg_tde` configuration, you need the following information:

    * The secret access token to the Vault server
    * The URL to access the Vault server
    * (Optional) The CA file used for SSL verification

* Use the local keyfile. This approach is rather used for development and testing purposes since the keys are stored unencrypted in the specified keyfile.

## Procedure 

Install `pg_tde` using one of available installation methods:


=== "Package manager" 

    Starting with Aplha1 version, you can install the extension as package from Percona repositories using the `percona-release` tool. The packages are available for the following operating systems:
    
    - Red Hat Enterprise Linux and CentOS 7
    - Red Hat Enterprise Linux 8 and compatible derivatives
    - Red Hat Enterprise Linux 9 and compatible derivatives
    - Ubuntu 20.04 (Focal Fossa)
    - Ubuntu 22.04 (Jammy Jellyfish)
    - Debian 10 (Buster)
    - Debian 11 (Bullseye) 
    - Debian 12 (Bookworm)

    [Install on Debian or Ubuntu](apt.md){.md-button}
    [Install on RHEL or derivatives](yum.md){.md-button}

=== "Build from source"

    1. To build `pg_tde` from source code, you require the following on Ubuntu/Debian:

        ```sh
        sudo apt install make gcc postgresql-server-dev-16 libcurl4-openssl-dev
        ```

    2. [Install Percona Distribution for PostgreSQL 16](https://docs.percona.com/postgresql/16/installing.html) or [upstream PostgreSQL 16](https://www.postgresql.org/download/)

    3. If PostgreSQL is installed in a non standard directory, set the `PG_CONFIG` environment variable to point to the `pg_config` executable.

    4. Clone the repository:  

        ```
        git clone git://github.com/Percona-Lab/pg_tde
        ```

    5. Compile and install the extension

        ```
        cd pg_tde
        ./configure
        make USE_PGXS=1
        sudo make USE_PGXS=1 install
        ```

=== "Run in Docker"

    You can find Docker images built from the current main branch on [Docker Hub](https://hub.docker.com/r/perconalab/pg_tde). Images are built on top of [postgres:16](https://hub.docker.com/_/postgres) official image.     

    To run `pg_tde` in Docker, use the following command:    

    ```
    docker run --name pg-tde -e POSTGRES_PASSWORD=mysecretpassword -d perconalab/pg_tde
    ```    

    It builds and adds `pg_tde` extension to PostgreSQL 16. Relevant `postgresql.conf` and `tde_conf.json` are created in `/etc/postgresql/` inside the container. This directory is exposed as a volume.    

    See [Docker Docs](https://hub.docker.com/_/postgres) on usage.    

    You can also build a Docker image manually with:    

    ```
    docker build . -f ./docker/Dockerfile -t your-image-name
    ```

## Next steps

[Setup](setup.md){.md-button}