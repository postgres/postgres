[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/percona/pg_tde/badge)](https://scorecard.dev/viewer/?uri=github.com/percona/pg_tde)
[![codecov](https://codecov.io/github/percona/postgres/graph/badge.svg?token=Wow78BMYdP)](https://codecov.io/github/percona/postgres)
[![Forum](https://img.shields.io/badge/Forum-join-brightgreen)](https://forums.percona.com/)
[![Docs](https://img.shields.io/badge/docs-pg_tde-blue)](https://docs.percona.com/pg-tde/)

# pg_tde: Transparent Database Encryption for PostgreSQL

The PostgreSQL extension provides data at rest encryption. It is currently in an experimental phase and is under active development. [We need your feedback!](https://github.com/percona/postgres/discussions)

## Table of Contents

1. [Overview](#overview)
2. [Documentation](#documentation)
3. [Percona Server for PostgreSQL](#percona-server-for-postgresql)
4. [Run in docker](#run-in-docker)
5. [Set up pg_tde](#set-up-pg_tde)
6. [Downloads](#downloads)
7. [Additional functions](#additional-functions)

## Overview

Transparent Data Encryption offers encryption at the file level and solves the problem of protecting data at rest. The encryption is transparent for users allowing them to access and manipulate the data and not to worry about the encryption process. The extension supports [keyringfile and external Key Management Systems (KMS) through a Global Key Provider interface](../pg_tde/documentation/docs/global-key-provider-configuration/index.md).

### This extension provides the `tde_heap access method`

This access method:

- Works only with [Percona Server for PostgreSQL 17](https://docs.percona.com/postgresql/17/postgresql-server.html)
- Uses extended Storage Manager and WAL APIs
- Encrypts tuples, WAL and indexes
- It **does not** encrypt temporary files and statistics **yet**

## Documentation

For more information about `pg_tde`, [see the official documentation](https://docs.percona.com/pg-tde/index.html).

## Percona Server for PostgreSQL

Percona provides binary packages of `pg_tde` extension only for Percona Server for PostgreSQL. Learn how to install them or build `pg_tde` from sources for PSPG in the [documentation](https://docs.percona.com/pg-tde/install.html).

## Run in Docker

To run `pg_tde` in Docker, follow the instructions in the [official pg_tde Docker documentation](https://docs.percona.com/postgresql/17/docker.html#enable-encryption).

_For details on the build process and developer setup, see [Make Builds for Developers](https://github.com/percona/pg_tde/wiki/Make-builds-for-developers)._

## Set up pg_tde

For more information on setting up and configuring `pg_tde`, see the [official pg_tde setup topic](https://docs.percona.com/pg-tde/setup.html).

The guide also includes instructions for:

- Installing and enabling the extension
- Setting up key providers
- Creating encrypted tables

## Downloads

To download the latest build of the main branch, use the `HEAD` release from [releases](https://github.com/percona/postgres/releases).

Builds are available in a tar.gz format, containing only the required files, and as a deb package.
The deb package is built against the pgdg17 release, but this dependency is not yet enforced in the package.

## Additional functions

Learn more about the helper functions available in `pg_tde`, including how to check table encryption status, in the [Functions topic](https://docs.percona.com/pg-tde/functions.html?h=pg_tde_is_encrypted#encryption-status-check).
