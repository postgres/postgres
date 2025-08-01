# Percona Transparent Data Encryption for PostgreSQL documentation

Percona Transparent Data Encryption for PostgreSQL (`pg_tde`) is an open source, community driven and futureproof PostgreSQL extension that provides Transparent Data Encryption (TDE) to protect data at rest. `pg_tde` ensures that the data stored on disk is encrypted, and that no one can read it without the proper encryption keys, even if they gain access to the physical storage media.

!!! warning "No upgrade path from RC to GA"
    There is no safe upgrade path from the previous versions, such as Release Candidate 2, to the General Availability (GA) version of `pg_tde`.  
    We recommend starting with a **clean installation** for GA deployments. Avoid using RC environments in production.

<div data-grid markdown><div data-banner markdown>

### :material-progress-download: Installation guide { .title }

Get started quickly with the step-by-step installation instructions.

[How to install `pg_tde` :material-arrow-right:](install.md){ .md-button }

</div><div data-banner markdown>

### :rocket: Features { .title }

Explore what features Percona's `pg_tde` extension brings to PostgreSQL.

[Check what you can do with `pg_tde` :material-arrow-right:](features.md){ .md-button }

</div><div data-banner markdown>

### :material-cog-refresh-outline: Architecture { .title }

Understand how `pg_tde` integrates into PostgreSQL with Percona's architecture. Learn how keys are managed, how encryption is applied, and how our design ensures performance and security.

[Check what’s under the hood for `pg_tde` :material-arrow-right:](architecture/architecture.md){.md-button}

</div><div data-banner markdown>

### :loudspeaker: What's new? { .title }

Learn about the releases and changes in `pg_tde`.

[Check what’s new in the latest version :material-arrow-right:](release-notes/{{latestreleasenotes}}.md){.md-button}
</div>
</div>
