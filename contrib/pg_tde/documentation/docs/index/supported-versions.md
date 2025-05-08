# Versions and Supported PostgreSQL Deployments

The `pg_tde` extension is available for [Percona Server for PostgreSQL 17.x](https://docs.percona.com/postgresql/17/postgresql-server.html), an open source, drop-in replacement for PostgreSQL Community. This version provides the `tde_heap` access method and offers [full encryption capabilities](../features.md), including encryption of tables, indexes, WAL data, and support for logical replication.

The extension is tightly integrated with Percona Server for PostgreSQL to deliver enhanced encryption functionality that is not available in community builds.

## Why choose Percona Server for PostgreSQL?

By using our PostgreSQL distribution, you get:

- **Full encryption support** through the `tde_heap` access method, including tables, indexes, WAL data, and logical replication.
- **Enhanced performance and enterprise-ready features** not available in community builds.
- **Regular updates and security patches** backed by Percona’s expert support team.
- **Professional support** and guidance for secure PostgreSQL deployments.

!!! note
    Support for earlier or limited versions of `pg_tde` (such as `tde_heap_basic`) has been deprecated.

Still unsure which deployment fits your needs? [Contact our experts](https://www.percona.com/about/contact) to find the best solution for your environment.

[Get Started: Install pg_tde :material-arrow-right:](../install.md){.md-button}
