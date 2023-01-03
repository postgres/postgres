#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#

import contextlib
import socket
import sys

import pytest

import pq3


@pytest.fixture
def connect():
    """
    A factory fixture that, when called, returns a socket connected to a
    Postgres server, wrapped in a pq3 connection. The calling test will be
    skipped automatically if a server is not running at PGHOST:PGPORT, so it's
    best to connect as soon as possible after the test case begins, to avoid
    doing unnecessary work.
    """
    # Set up an ExitStack to handle safe cleanup of all of the moving pieces.
    with contextlib.ExitStack() as stack:

        def conn_factory():
            addr = (pq3.pghost(), pq3.pgport())

            try:
                sock = socket.create_connection(addr, timeout=2)
            except ConnectionError as e:
                pytest.skip(f"unable to connect to {addr}: {e}")

            # Have ExitStack close our socket.
            stack.enter_context(sock)

            # Wrap the connection in a pq3 layer and have ExitStack clean it up
            # too.
            wrap_ctx = pq3.wrap(sock, debug_stream=sys.stdout)
            conn = stack.enter_context(wrap_ctx)

            return conn

        yield conn_factory
