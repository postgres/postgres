# Switch from Percona Server for PostgreSQL to PostgreSQL Community

Percona Server for PostgreSQL and PostgreSQL Community are binary compatible and enable you to switch from one to another. Here's how:

1. If you used the `tde_heap` (tech preview feature) access method for encryption, either re-encrypt the data using the `tde_heap_basic` access method, or [decrypt](decrypt.md) it completely 