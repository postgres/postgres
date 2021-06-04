#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#

import contextlib
import datetime
import functools
import ipaddress
import os
import socket
import sys
import threading

import psycopg2
import psycopg2.extras
import pytest
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

import pq3

BLOCKING_TIMEOUT = 2  # the number of seconds to wait for blocking calls


@pytest.fixture
def server_socket(unused_tcp_port_factory):
    """
    Returns a listening socket bound to an ephemeral port.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", unused_tcp_port_factory()))
        s.listen(1)
        s.settimeout(BLOCKING_TIMEOUT)
        yield s


class ClientHandshake(threading.Thread):
    """
    A thread that connects to a local Postgres server using psycopg2. Once the
    opening handshake completes, the connection will be immediately closed.
    """

    def __init__(self, *, port, **kwargs):
        super().__init__()

        kwargs["port"] = port
        self._kwargs = kwargs

        self.exception = None

    def run(self):
        try:
            conn = psycopg2.connect(host="127.0.0.1", **self._kwargs)
            with contextlib.closing(conn):
                self._pump_async(conn)
        except Exception as e:
            self.exception = e

    def check_completed(self, timeout=BLOCKING_TIMEOUT):
        """
        Joins the client thread. Raises an exception if the thread could not be
        joined, or if it threw an exception itself. (The exception will be
        cleared, so future calls to check_completed will succeed.)
        """
        self.join(timeout)

        if self.is_alive():
            raise TimeoutError("client thread did not handshake within the timeout")
        elif self.exception:
            e = self.exception
            self.exception = None
            raise e

    def _pump_async(self, conn):
        """
        Polls a psycopg2 connection until it's completed. (Synchronous
        connections will work here too; they'll just immediately return OK.)
        """
        psycopg2.extras.wait_select(conn)


@pytest.fixture
def accept(server_socket):
    """
    Returns a factory function that, when called, returns a pair (sock, client)
    where sock is a server socket that has accepted a connection from client,
    and client is an instance of ClientHandshake. Clients will complete their
    handshakes and cleanly disconnect.

    The default connstring options may be extended or overridden by passing
    arbitrary keyword arguments. Keep in mind that you generally should not
    override the host or port, since they point to the local test server.

    For situations where a client needs to connect more than once to complete a
    handshake, the accept function may be called more than once. (The client
    returned for subsequent calls will always be the same client that was
    returned for the first call.)

    Tests must either complete the handshake so that the client thread can be
    automatically joined during teardown, or else call client.check_completed()
    and manually handle any expected errors.
    """
    _, port = server_socket.getsockname()

    client = None
    default_opts = dict(
        port=port,
        user=pq3.pguser(),
        sslmode="disable",
    )

    def factory(**kwargs):
        nonlocal client

        if client is None:
            opts = dict(default_opts)
            opts.update(kwargs)

            # The server_socket is already listening, so the client thread can
            # be safely started; it'll block on the connection until we accept.
            client = ClientHandshake(**opts)
            client.start()

        sock, _ = server_socket.accept()
        sock.settimeout(BLOCKING_TIMEOUT)
        return sock, client

    yield factory

    if client is not None:
        client.check_completed()


@pytest.fixture
def conn(accept):
    """
    Returns an accepted, wrapped pq3 connection to a psycopg2 client. The socket
    will be closed when the test finishes, and the client will be checked for a
    cleanly completed handshake.
    """
    sock, client = accept()
    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            yield conn


@pytest.fixture(scope="session")
def certpair(tmp_path_factory):
    """
    Yields a (cert, key) pair of file paths that can be used by a TLS server.
    The certificate is issued for "localhost" and its standard IPv4/6 addresses.
    """

    tmpdir = tmp_path_factory.mktemp("certs")
    now = datetime.datetime.now(datetime.timezone.utc)

    # https://cryptography.io/en/latest/x509/tutorial/#creating-a-self-signed-certificate
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)

    subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    altNames = [
        x509.DNSName("localhost"),
        x509.IPAddress(ipaddress.IPv4Address("127.0.0.1")),
        x509.IPAddress(ipaddress.IPv6Address("::1")),
    ]
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(now + datetime.timedelta(minutes=10))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .add_extension(x509.SubjectAlternativeName(altNames), critical=False)
    ).sign(key, hashes.SHA256())

    # Writing the key with mode 0600 lets us use this from the server side, too.
    keypath = str(tmpdir / "key.pem")
    with open(keypath, "wb", opener=functools.partial(os.open, mode=0o600)) as f:
        f.write(
            key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.PKCS8,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )

    certpath = str(tmpdir / "cert.pem")
    with open(certpath, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))

    return certpath, keypath
