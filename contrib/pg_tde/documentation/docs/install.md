# Install pg_tde

!!! warning "No upgrade path from RC to GA"
    There is no safe upgrade path from the previous versions, such as Release Candidate 2, to the General Availability (GA) version of `pg_tde`.  
    We recommend starting with a **clean installation** for GA deployments. Avoid using RC environments in production.

To install `pg_tde`, use one of the following methods:

=== ":octicons-terminal-16: Package manager"

    The packages are available for the following operating systems:
    
    - Red Hat Enterprise Linux 8 and compatible derivatives
    - Red Hat Enterprise Linux 9 and compatible derivatives
    - Ubuntu 20.04 (Focal Fossa)
    - Ubuntu 22.04 (Jammy Jellyfish)
    - Ubuntu 24.04 (Noble Numbat)
    - Debian 11 (Bullseye) 
    - Debian 12 (Bookworm)

    [Install on Debian or Ubuntu :material-arrow-right:](apt.md){.md-button}
    [Install on RHEL or derivatives :material-arrow-right:](yum.md){.md-button}

=== ":simple-docker: Docker"

    `pg_tde` is a part of the Percona Distribution for PostgreSQL Docker image. Use this image to enjoy full encryption capabilities. Check below to get access to a detailed step-by-step guide. 

    [Run in Docker :material-arrow-right:](https://docs.percona.com/postgresql/latest/docker.html){.md-button}

=== ":octicons-download-16: Tar download"

    `pg_tde` is included in the Percona Distribution for PostgreSQL tarball. Select the below link to access the step-by-step guide. 

    [Install from tarballs :material-arrow-right:](https://docs.percona.com/postgresql/17/tarball.html){.md-button}

Follow the configuration steps below to continue:

[Configure pg_tde :material-arrow-right:](setup.md){.md-button}

If you’ve already completed these steps, feel free to skip ahead to a later section:

 [Configure Key Management (KMS)](global-key-provider-configuration/overview.md){.md-button} [Validate Encryption with pg_tde](test.md){.md-button} [Configure WAL encryption](wal-encryption.md){.md-button}
