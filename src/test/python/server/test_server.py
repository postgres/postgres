#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#

import pq3


def test_handshake(connect):
    """Basic sanity check."""
    conn = connect()

    pq3.handshake(conn, user=pq3.pguser(), database=pq3.pgdatabase())

    pq3.send(conn, pq3.types.Query, query=b"")

    resp = pq3.recv1(conn)
    assert resp.type == pq3.types.EmptyQueryResponse

    resp = pq3.recv1(conn)
    assert resp.type == pq3.types.ReadyForQuery
