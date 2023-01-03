#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#

import base64
import sys

import psycopg2
import pytest
from cryptography.hazmat.primitives import hashes, hmac

import pq3


def finish_handshake(conn):
    """
    Sends the AuthenticationOK message and the standard opening salvo of server
    messages, then asserts that the client immediately sends a Terminate message
    to close the connection cleanly.
    """
    pq3.send(conn, pq3.types.AuthnRequest, type=pq3.authn.OK)
    pq3.send(conn, pq3.types.ParameterStatus, name=b"client_encoding", value=b"UTF-8")
    pq3.send(conn, pq3.types.ParameterStatus, name=b"DateStyle", value=b"ISO, MDY")
    pq3.send(conn, pq3.types.ReadyForQuery, status=b"I")

    pkt = pq3.recv1(conn)
    assert pkt.type == pq3.types.Terminate


def test_handshake(conn):
    startup = pq3.recv1(conn, cls=pq3.Startup)
    assert startup.proto == pq3.protocol(3, 0)

    finish_handshake(conn)


def test_aborted_connection(accept):
    """
    Make sure the client correctly reports an early close during handshakes.
    """
    sock, client = accept()
    sock.close()

    expected = "server closed the connection unexpectedly"
    with pytest.raises(psycopg2.OperationalError, match=expected):
        client.check_completed()


#
# SCRAM-SHA-256 (see RFC 5802: https://tools.ietf.org/html/rfc5802)
#


@pytest.fixture
def password():
    """
    Returns a password for use by both client and server.
    """
    # TODO: parameterize this with passwords that require SASLprep.
    return "secret"


@pytest.fixture
def pwconn(accept, password):
    """
    Like the conn fixture, but uses a password in the connection.
    """
    sock, client = accept(password=password)
    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            yield conn


def sha256(data):
    """The H(str) function from Section 2.2."""
    digest = hashes.Hash(hashes.SHA256())
    digest.update(data)
    return digest.finalize()


def hmac_256(key, data):
    """The HMAC(key, str) function from Section 2.2."""
    h = hmac.HMAC(key, hashes.SHA256())
    h.update(data)
    return h.finalize()


def xor(a, b):
    """The XOR operation from Section 2.2."""
    res = bytearray(a)
    for i, byte in enumerate(b):
        res[i] ^= byte
    return bytes(res)


def h_i(data, salt, i):
    """The Hi(str, salt, i) function from Section 2.2."""
    assert i > 0

    acc = hmac_256(data, salt + b"\x00\x00\x00\x01")
    last = acc
    i -= 1

    while i:
        u = hmac_256(data, last)
        acc = xor(acc, u)

        last = u
        i -= 1

    return acc


def test_scram(pwconn, password):
    startup = pq3.recv1(pwconn, cls=pq3.Startup)
    assert startup.proto == pq3.protocol(3, 0)

    pq3.send(
        pwconn,
        pq3.types.AuthnRequest,
        type=pq3.authn.SASL,
        body=[b"SCRAM-SHA-256", b""],
    )

    # Get the client-first-message.
    pkt = pq3.recv1(pwconn)
    assert pkt.type == pq3.types.PasswordMessage

    initial = pq3.SASLInitialResponse.parse(pkt.payload)
    assert initial.name == b"SCRAM-SHA-256"

    c_bind, authzid, c_name, c_nonce = initial.data.split(b",")
    assert c_bind == b"n"  # no channel bindings on a plaintext connection
    assert authzid == b""  # we don't support authzid currently
    assert c_name == b"n="  # libpq doesn't honor the GS2 username
    assert c_nonce.startswith(b"r=")

    # Send the server-first-message.
    salt = b"12345"
    iterations = 2

    s_nonce = c_nonce + b"somenonce"
    s_salt = b"s=" + base64.b64encode(salt)
    s_iterations = b"i=%d" % iterations

    msg = b",".join([s_nonce, s_salt, s_iterations])
    pq3.send(pwconn, pq3.types.AuthnRequest, type=pq3.authn.SASLContinue, body=msg)

    # Get the client-final-message.
    pkt = pq3.recv1(pwconn)
    assert pkt.type == pq3.types.PasswordMessage

    c_bind_final, c_nonce_final, c_proof = pkt.payload.split(b",")
    assert c_bind_final == b"c=" + base64.b64encode(c_bind + b"," + authzid + b",")
    assert c_nonce_final == s_nonce

    # Calculate what the client proof should be.
    salted_password = h_i(password.encode("ascii"), salt, iterations)
    client_key = hmac_256(salted_password, b"Client Key")
    stored_key = sha256(client_key)

    auth_message = b",".join(
        [c_name, c_nonce, s_nonce, s_salt, s_iterations, c_bind_final, c_nonce_final]
    )
    client_signature = hmac_256(stored_key, auth_message)
    client_proof = xor(client_key, client_signature)

    expected = b"p=" + base64.b64encode(client_proof)
    assert c_proof == expected

    # Send the correct server signature.
    server_key = hmac_256(salted_password, b"Server Key")
    server_signature = hmac_256(server_key, auth_message)

    s_verify = b"v=" + base64.b64encode(server_signature)
    pq3.send(pwconn, pq3.types.AuthnRequest, type=pq3.authn.SASLFinal, body=s_verify)

    # Done!
    finish_handshake(pwconn)
