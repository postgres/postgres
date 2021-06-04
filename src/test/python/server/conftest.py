#
# Portions Copyright 2021 VMware, Inc.
# Portions Copyright 2023 Timescale, Inc.
# SPDX-License-Identifier: PostgreSQL
#

import collections
import contextlib
import os
import shutil
import socket
import subprocess
import sys

import pytest

import pq3

BLOCKING_TIMEOUT = 2  # the number of seconds to wait for blocking calls


def cleanup_prior_instance(datadir):
    """
    Clean up an existing data directory, but make sure it actually looks like a
    data directory first. (Empty folders will remain untouched, since initdb can
    populate them.)
    """
    required_entries = set(["base", "PG_VERSION", "postgresql.conf"])
    empty = True

    try:
        with os.scandir(datadir) as entries:
            for e in entries:
                empty = False
                required_entries.discard(e.name)

    except FileNotFoundError:
        return  # nothing to clean up

    if empty:
        return  # initdb can handle an empty datadir

    if required_entries:
        pytest.fail(
            f"--temp-instance directory \"{datadir}\" is not empty and doesn't look like a data directory (missing {', '.join(required_entries)})"
        )

    # Okay, seems safe enough now.
    shutil.rmtree(datadir)


@pytest.fixture(scope="session")
def postgres_instance(pytestconfig, unused_tcp_port_factory):
    """
    If --temp-instance has been passed to pytest, this fixture runs a temporary
    Postgres instance on an available port. Otherwise, the fixture will attempt
    to contact a running Postgres server on (PGHOST, PGPORT); dependent tests
    will be skipped if the connection fails.

    Yields a (host, port) tuple for connecting to the server.
    """
    PGInstance = collections.namedtuple("PGInstance", ["addr", "temporary"])

    datadir = pytestconfig.getoption("temp_instance")
    if datadir:
        # We were told to create a temporary instance. Use pg_ctl to set it up
        # on an unused port.
        cleanup_prior_instance(datadir)
        subprocess.run(["pg_ctl", "-D", datadir, "init"], check=True)

        # The CI looks for *.log files to upload, so the file name here isn't
        # completely arbitrary.
        log = os.path.join(datadir, "postmaster.log")
        port = unused_tcp_port_factory()

        subprocess.run(
            [
                "pg_ctl",
                "-D",
                datadir,
                "-l",
                log,
                "-o",
                " ".join(
                    [
                        f"-c port={port}",
                        "-c listen_addresses=localhost",
                        "-c log_connections=on",
                        "-c session_preload_libraries=oauthtest",
                        "-c oauth_validator_libraries=oauthtest",
                    ]
                ),
                "start",
            ],
            check=True,
        )

        yield ("localhost", port)

        subprocess.run(["pg_ctl", "-D", datadir, "stop"], check=True)

    else:
        # Try to contact an already running server; skip the suite if we can't
        # find one.
        addr = (pq3.pghost(), pq3.pgport())

        try:
            with socket.create_connection(addr, timeout=BLOCKING_TIMEOUT):
                pass
        except ConnectionError as e:
            pytest.skip(f"unable to connect to Postgres server at {addr}: {e}")

        yield addr


@pytest.fixture
def connect(postgres_instance):
    """
    A factory fixture that, when called, returns a socket connected to a
    Postgres server, wrapped in a pq3 connection. Dependent tests will be
    skipped if no server is available.
    """
    addr = postgres_instance

    # Set up an ExitStack to handle safe cleanup of all of the moving pieces.
    with contextlib.ExitStack() as stack:

        def conn_factory():
            sock = socket.create_connection(addr, timeout=BLOCKING_TIMEOUT)

            # Have ExitStack close our socket.
            stack.enter_context(sock)

            # Wrap the connection in a pq3 layer and have ExitStack clean it up
            # too.
            wrap_ctx = pq3.wrap(sock, debug_stream=sys.stdout)
            conn = stack.enter_context(wrap_ctx)

            return conn

        yield conn_factory
