# Percona Transparent Data Encryption for PostgreSQL documentation

Percona Transparent Data Encryption for PostgreSQL (`pg_tde`) is an open source, community driven and futureproof PostgreSQL extension that provides Transparent Data Encryption (TDE) to protect data at rest. `pg_tde` ensures that the data stored on disk is encrypted, and that no one can read it without the proper encryption keys, even if they gain access to the physical storage media.

!!! warning "No upgrade path from RC to GA"
    There is no safe upgrade path from the previous versions, such as Release Candidate 2, to the General Availability (GA) version of `pg_tde`.  
    We recommend starting with a **clean installation** for GA deployments. Avoid using RC environments in production.

[Learn more about TDE and pg_tde :material-arrow-right:](index/about-tde.md){.md-button}
[Get started with installation :material-arrow-right:](install.md){.md-button}
[Check out what's new in pg_tde {{release}} :material-arrow-right:](release-notes/release-notes.md){.md-button}
