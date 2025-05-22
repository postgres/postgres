#
# Copyright 2021 VMware, Inc.
# Portions Copyright 2023 Timescale, Inc.
# Portions Copyright 2024 PostgreSQL Global Development Group
# SPDX-License-Identifier: PostgreSQL
#

import base64
import collections
import contextlib
import ctypes
import http.server
import json
import logging
import os
import platform
import secrets
import socket
import ssl
import sys
import threading
import time
import traceback
import types
import urllib.parse
from numbers import Number

import psycopg2
import pytest

import pq3

from .conftest import BLOCKING_TIMEOUT

# The client tests need libpq to have been compiled with OAuth support; skip
# them otherwise.
pytestmark = pytest.mark.skipif(
    os.getenv("with_libcurl") != "yes",
    reason="OAuth client tests require --with-libcurl support",
)

if platform.system() == "Darwin":
    libpq = ctypes.cdll.LoadLibrary("libpq.5.dylib")
elif platform.system() == "Windows":
    pass  # TODO
else:
    libpq = ctypes.cdll.LoadLibrary("libpq.so.5")


def finish_handshake(conn):
    """
    Sends the AuthenticationOK message and the standard opening salvo of server
    messages, then asserts that the client immediately sends a Terminate message
    to close the connection cleanly.
    """
    pq3.send(conn, pq3.types.AuthnRequest, type=pq3.authn.OK)
    pq3.send(conn, pq3.types.ParameterStatus, name=b"client_encoding", value=b"UTF-8")
    pq3.send(conn, pq3.types.ParameterStatus, name=b"DateStyle", value=b"ISO, MDY")
    pq3.send(conn, pq3.types.BackendKeyData, pid=1234, key=0)
    pq3.send(conn, pq3.types.ReadyForQuery, status=b"I")

    pkt = pq3.recv1(conn)
    assert pkt.type == pq3.types.Terminate


#
# OAUTHBEARER (see RFC 7628: https://tools.ietf.org/html/rfc7628)
#


def start_oauth_handshake(conn):
    """
    Negotiates an OAUTHBEARER SASL challenge. Returns the client's initial
    response data.
    """
    startup = pq3.recv1(conn, cls=pq3.Startup)
    assert startup.proto == pq3.protocol(3, 0)

    pq3.send(
        conn, pq3.types.AuthnRequest, type=pq3.authn.SASL, body=[b"OAUTHBEARER", b""]
    )

    pkt = pq3.recv1(conn)
    assert pkt.type == pq3.types.PasswordMessage

    initial = pq3.SASLInitialResponse.parse(pkt.payload)
    assert initial.name == b"OAUTHBEARER"

    return initial.data


def get_auth_value(initial):
    """
    Finds the auth value (e.g. "Bearer somedata..." in the client's initial SASL
    response.
    """
    kvpairs = initial.split(b"\x01")
    assert kvpairs[0] == b"n,,"  # no channel binding or authzid
    assert kvpairs[2] == b""  # ends with an empty kvpair
    assert kvpairs[3] == b""  # ...and there's nothing after it
    assert len(kvpairs) == 4

    key, value = kvpairs[1].split(b"=", 2)
    assert key == b"auth"

    return value


def fail_oauth_handshake(conn, sasl_resp, *, errmsg="doesn't matter"):
    """
    Sends a failure response via the OAUTHBEARER mechanism, consumes the
    client's dummy response, and issues a FATAL error to end the exchange.

    sasl_resp is a dictionary which will be serialized as the OAUTHBEARER JSON
    response. If provided, errmsg is used in the FATAL ErrorResponse.
    """
    resp = json.dumps(sasl_resp)
    pq3.send(
        conn,
        pq3.types.AuthnRequest,
        type=pq3.authn.SASLContinue,
        body=resp.encode("utf-8"),
    )

    # Per RFC, the client is required to send a dummy ^A response.
    pkt = pq3.recv1(conn)
    assert pkt.type == pq3.types.PasswordMessage
    assert pkt.payload == b"\x01"

    # Now fail the SASL exchange.
    pq3.send(
        conn,
        pq3.types.ErrorResponse,
        fields=[
            b"SFATAL",
            b"C28000",
            b"M" + errmsg.encode("utf-8"),
            b"",
        ],
    )


def handle_discovery_connection(sock, discovery=None, *, response=None):
    """
    Helper for all tests that expect an initial discovery connection from the
    client. The provided discovery URI will be used in a standard error response
    from the server (or response may be set, to provide a custom dictionary),
    and the SASL exchange will be failed.

    By default, the client is expected to complete the entire handshake. Set
    finish to False if the client should immediately disconnect when it receives
    the error response.
    """
    if response is None:
        response = {"status": "invalid_token"}
        if discovery is not None:
            response["openid-configuration"] = discovery

    with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
        # Initiate a handshake.
        initial = start_oauth_handshake(conn)

        # For discovery, the client should send an empty auth header. See RFC
        # 7628, Sec. 4.3.
        auth = get_auth_value(initial)
        assert auth == b""

        # The discovery handshake is doomed to fail.
        fail_oauth_handshake(conn, response)


class RawResponse(str):
    """
    Returned by registered endpoint callbacks to take full control of the
    response. Usually, return values are converted to JSON; a RawResponse body
    will be passed to the client as-is, allowing endpoint implementations to
    issue invalid JSON.
    """

    pass


class RawBytes(bytes):
    """
    Like RawResponse, but bypasses the UTF-8 encoding step as well, allowing
    implementations to issue invalid encodings.
    """

    pass


class OpenIDProvider(threading.Thread):
    """
    A thread that runs a mock OpenID provider server on an SSL-enabled socket.
    """

    def __init__(self, ssl_socket):
        super().__init__()

        self.exception = None

        port = ssl_socket.getsockname()[1]
        oauth = self._OAuthState()

        if socket.has_dualstack_ipv6():
            oauth.host = f"localhost:{port}"
            oauth.issuer = f"https://localhost:{port}"
        else:
            oauth.host = f"127.0.0.1:{port}"
            oauth.issuer = f"https://127.0.0.1:{port}"

        # The following endpoints are required to be advertised by providers,
        # even though our chosen client implementation does not actually make
        # use of them.
        oauth.register_endpoint(
            "authorization_endpoint", "POST", "/authorize", self._authorization_handler
        )
        oauth.register_endpoint("jwks_uri", "GET", "/keys", self._jwks_handler)

        self.server = self._HTTPSServer(ssl_socket, self._Handler)
        self.server.oauth = oauth

    def run(self):
        try:
            # XXX socketserver.serve_forever() has a serious architectural
            # issue: its select loop wakes up every `poll_interval` seconds to
            # see if the server is shutting down. The default, 500 ms, only lets
            # us run two tests every second. But the faster we go, the more CPU
            # we burn unnecessarily...
            self.server.serve_forever(poll_interval=0.01)
        except Exception as e:
            self.exception = e

    def stop(self, timeout=BLOCKING_TIMEOUT):
        """
        Shuts down the server and joins its thread. Raises an exception if the
        thread could not be joined, or if it threw an exception itself. Must
        only be called once, after start().
        """
        self.server.shutdown()
        self.join(timeout)

        if self.is_alive():
            raise TimeoutError("client thread did not handshake within the timeout")
        elif self.exception:
            e = self.exception
            raise e

    class _OAuthState(object):
        def __init__(self):
            self.endpoint_paths = {}
            self._endpoints = {}

            # Provide a standard discovery document by default; tests can
            # override it.
            self.register_endpoint(
                None,
                "GET",
                "/.well-known/openid-configuration",
                self._default_discovery_handler,
            )

            # Default content type unless overridden.
            self.content_type = "application/json"

        @property
        def discovery_uri(self):
            return f"{self.issuer}/.well-known/openid-configuration"

        def register_endpoint(self, name, method, path, func):
            if method not in self._endpoints:
                self._endpoints[method] = {}

            self._endpoints[method][path] = func

            if name is not None:
                self.endpoint_paths[name] = path

        def endpoint(self, method, path):
            if method not in self._endpoints:
                return None

            return self._endpoints[method].get(path)

        def _default_discovery_handler(self, headers, params):
            doc = {
                "issuer": self.issuer,
                "response_types_supported": ["token"],
                "subject_types_supported": ["public"],
                "id_token_signing_alg_values_supported": ["RS256"],
                "grant_types_supported": [
                    "authorization_code",
                    "urn:ietf:params:oauth:grant-type:device_code",
                ],
            }

            for name, path in self.endpoint_paths.items():
                doc[name] = self.issuer + path

            return 200, doc

    class _HTTPSServer(http.server.HTTPServer):
        def __init__(self, ssl_socket, handler_cls):
            # Attach the SSL socket to the server. We don't bind/activate since
            # the socket is already listening.
            super().__init__(None, handler_cls, bind_and_activate=False)
            self.socket = ssl_socket
            self.server_address = self.socket.getsockname()

        def shutdown_request(self, request):
            # Cleanly unwrap the SSL socket before shutting down the connection;
            # otherwise careful clients will complain about truncation.
            try:
                request = request.unwrap()
            except (ssl.SSLEOFError, ConnectionResetError, BrokenPipeError):
                # The client already closed (or aborted) the connection without
                # a clean shutdown. This is seen on some platforms during tests
                # that break the HTTP protocol. Just return and have the server
                # close the socket.
                return
            except ssl.SSLError as err:
                # FIXME OpenSSL 3.4 introduced an incompatibility with Python's
                # TLS error handling, resulting in a bogus "[SYS] unknown error"
                # on some platforms. Hopefully this is fixed in 2025's set of
                # maintenance releases and this case can be removed.
                #
                #     https://github.com/python/cpython/issues/127257
                #
                if "[SYS] unknown error" in str(err):
                    return
                raise

            super().shutdown_request(request)

        def handle_error(self, request, addr):
            self.shutdown_request(request)
            raise

    @staticmethod
    def _jwks_handler(headers, params):
        return 200, {"keys": []}

    @staticmethod
    def _authorization_handler(headers, params):
        # We don't actually want this to be called during these tests -- we
        # should be using the device authorization endpoint instead.
        assert (
            False
        ), "authorization handler called instead of device authorization handler"

    class _Handler(http.server.BaseHTTPRequestHandler):
        timeout = BLOCKING_TIMEOUT

        def _handle(self, *, params=None, handler=None):
            oauth = self.server.oauth
            assert self.headers["Host"] == oauth.host

            # XXX: BaseHTTPRequestHandler collapses leading slashes in the path
            # to work around an open redirection vuln (gh-87389) in
            # SimpleHTTPServer. But we're not using SimpleHTTPServer, and we
            # want to test repeating leading slashes, so that's not very
            # helpful. Put them back.
            orig_path = self.raw_requestline.split()[1]
            orig_path = str(orig_path, "iso-8859-1")
            assert orig_path.endswith(self.path)  # sanity check
            self.path = orig_path

            if handler is None:
                handler = oauth.endpoint(self.command, self.path)
                assert (
                    handler is not None
                ), f"no registered endpoint for {self.command} {self.path}"

            result = handler(self.headers, params)

            if len(result) == 2:
                headers = {"Content-Type": oauth.content_type}
                code, resp = result
            else:
                code, headers, resp = result

            self.send_response(code)
            for h, v in headers.items():
                self.send_header(h, v)
            self.end_headers()

            if resp is not None:
                if not isinstance(resp, RawBytes):
                    if not isinstance(resp, RawResponse):
                        resp = json.dumps(resp)
                    resp = resp.encode("utf-8")
                self.wfile.write(resp)

            self.close_connection = True

        def do_GET(self):
            self._handle()

        def _request_body(self):
            length = self.headers["Content-Length"]

            # Handle only an explicit content-length.
            assert length is not None
            length = int(length)

            return self.rfile.read(length).decode("utf-8")

        def do_POST(self):
            assert self.headers["Content-Type"] == "application/x-www-form-urlencoded"

            body = self._request_body()
            if body:
                # parse_qs() is understandably fairly lax when it comes to
                # acceptable characters, but we're stricter. Spaces must be
                # encoded, and they must use the '+' encoding rather than "%20".
                assert " " not in body
                assert "%20" not in body

                params = urllib.parse.parse_qs(
                    body,
                    keep_blank_values=True,
                    strict_parsing=True,
                    encoding="utf-8",
                    errors="strict",
                )
            else:
                params = {}

            self._handle(params=params)


@pytest.fixture(autouse=True)
def enable_client_oauth_debugging(monkeypatch):
    """
    HTTP providers aren't allowed by default; enable them via envvar.
    """
    monkeypatch.setenv("PGOAUTHDEBUG", "UNSAFE")


@pytest.fixture(autouse=True)
def trust_certpair_in_client(monkeypatch, certpair):
    """
    Set a trusted CA file for OAuth client connections.
    """
    monkeypatch.setenv("PGOAUTHCAFILE", certpair[0])


@pytest.fixture(scope="session")
def ssl_socket(certpair):
    """
    A listening server-side socket for SSL connections, using the certpair
    fixture.
    """
    # Try to listen on both IPv4 and v6, if possible, for extra coverage of Curl
    # corner cases compared to the standard test suite. Otherwise just use IPv4.
    if socket.has_dualstack_ipv6():
        sock = socket.create_server(
            ("", 0), family=socket.AF_INET6, dualstack_ipv6=True
        )
    else:
        sock = socket.create_server(("", 0))

    # The TLS connections we're making are incredibly sensitive to delayed ACKs
    # from the client. (Without TCP_NODELAY, test performance degrades 4-5x.)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    with contextlib.closing(sock):
        # Wrap the server socket for TLS.
        ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
        ctx.load_cert_chain(*certpair)

        yield ctx.wrap_socket(sock, server_side=True)


@pytest.fixture
def openid_provider(ssl_socket):
    """
    A fixture that returns the OAuth state of a running OpenID provider server. The
    server will be stopped when the fixture is torn down.
    """
    thread = OpenIDProvider(ssl_socket)
    thread.start()

    try:
        yield thread.server.oauth
    finally:
        thread.stop()


#
# PQAuthDataHook implementation, matching libpq.h
#


PQAUTHDATA_PROMPT_OAUTH_DEVICE = 0
PQAUTHDATA_OAUTH_BEARER_TOKEN = 1

PGRES_POLLING_FAILED = 0
PGRES_POLLING_READING = 1
PGRES_POLLING_WRITING = 2
PGRES_POLLING_OK = 3


class PGPromptOAuthDevice(ctypes.Structure):
    _fields_ = [
        ("verification_uri", ctypes.c_char_p),
        ("user_code", ctypes.c_char_p),
        ("verification_uri_complete", ctypes.c_char_p),
        ("expires_in", ctypes.c_int),
    ]


class PGOAuthBearerRequest(ctypes.Structure):
    pass


PGOAuthBearerRequest._fields_ = [
    ("openid_configuration", ctypes.c_char_p),
    ("scope", ctypes.c_char_p),
    (
        "async_",
        ctypes.CFUNCTYPE(
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.POINTER(PGOAuthBearerRequest),
            ctypes.POINTER(ctypes.c_int),
        ),
    ),
    (
        "cleanup",
        ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.POINTER(PGOAuthBearerRequest)),
    ),
    ("token", ctypes.c_char_p),
    ("user", ctypes.c_void_p),
]


@pytest.fixture
def auth_data_cb():
    """
    Tracks calls to the libpq authdata hook. The yielded object contains a calls
    member that records the data sent to the hook. If a test needs to perform
    custom actions during a call, it can set the yielded object's impl callback;
    beware that the callback takes place on a different thread.

    This is done differently from the other callback implementations on purpose.
    For the others, we can declare test-specific callbacks and have them perform
    direct assertions on the data they receive. But that won't work for a C
    callback, because there's no way for us to bubble up the assertion through
    libpq. Instead, this mock-style approach is taken, where we just record the
    calls and let the test examine them later.
    """

    class _Call:
        pass

    class _cb(object):
        def __init__(self):
            self.calls = []

    cb = _cb()
    cb.impl = None

    # The callback will occur on a different thread, so protect the cb object.
    cb_lock = threading.Lock()

    @ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_byte, ctypes.c_void_p, ctypes.c_void_p)
    def auth_data_cb(typ, pgconn, data):
        handle_by_default = 0  # does an implementation have to be provided?

        if typ == PQAUTHDATA_PROMPT_OAUTH_DEVICE:
            cls = PGPromptOAuthDevice
            handle_by_default = 1
        elif typ == PQAUTHDATA_OAUTH_BEARER_TOKEN:
            cls = PGOAuthBearerRequest
        else:
            return 0

        call = _Call()
        call.type = typ

        # The lifetime of the underlying data being pointed to doesn't
        # necessarily match the lifetime of the Python object, so we can't
        # reference a Structure's fields after returning. Explicitly copy the
        # contents over, field by field.
        data = ctypes.cast(data, ctypes.POINTER(cls))
        for name, _ in cls._fields_:
            setattr(call, name, getattr(data.contents, name))

        with cb_lock:
            cb.calls.append(call)

        if cb.impl:
            # Pass control back to the test.
            try:
                return cb.impl(typ, pgconn, data.contents)
            except Exception:
                # This can't escape into the C stack, but we can fail the flow
                # and hope the traceback gives us enough detail.
                logging.error(
                    "Exception during authdata hook callback:\n"
                    + traceback.format_exc()
                )
                return -1

        return handle_by_default

    libpq.PQsetAuthDataHook(auth_data_cb)
    try:
        yield cb
    finally:
        # The callback is about to go out of scope, so make sure libpq is
        # disconnected from it. (We wouldn't want to accidentally influence
        # later tests anyway.)
        libpq.PQsetAuthDataHook(None)


@pytest.mark.parametrize(
    "success, abnormal_failure",
    [
        pytest.param(True, False, id="success"),
        pytest.param(False, False, id="normal failure"),
        pytest.param(False, True, id="abnormal failure"),
    ],
)
@pytest.mark.parametrize("secret", [None, "", "hunter2"])
@pytest.mark.parametrize("scope", [None, "", "openid email"])
@pytest.mark.parametrize("retries", [0, 1])
@pytest.mark.parametrize(
    "content_type",
    [
        pytest.param("application/json", id="standard"),
        pytest.param("application/json;charset=utf-8", id="charset"),
        pytest.param("application/json \t;\t charset=utf-8", id="charset (whitespace)"),
    ],
)
@pytest.mark.parametrize("uri_spelling", ["verification_url", "verification_uri"])
@pytest.mark.parametrize(
    "asynchronous",
    [
        pytest.param(False, id="synchronous"),
        pytest.param(True, id="asynchronous"),
    ],
)
def test_oauth_with_explicit_discovery_uri(
    accept,
    openid_provider,
    asynchronous,
    uri_spelling,
    content_type,
    retries,
    scope,
    secret,
    auth_data_cb,
    success,
    abnormal_failure,
):
    client_id = secrets.token_hex()
    openid_provider.content_type = content_type

    sock, client = accept(
        oauth_issuer=openid_provider.discovery_uri,
        oauth_client_id=client_id,
        oauth_client_secret=secret,
        oauth_scope=scope,
        async_=asynchronous,
    )

    device_code = secrets.token_hex()
    user_code = f"{secrets.token_hex(2)}-{secrets.token_hex(2)}"
    verification_url = "https://example.com/device"

    access_token = secrets.token_urlsafe()

    def check_client_authn(headers, params):
        if secret is None:
            assert "Authorization" not in headers
            assert params["client_id"] == [client_id]
            return

        # Require the client to use Basic authn; request-body credentials are
        # NOT RECOMMENDED (RFC 6749, Sec. 2.3.1).
        assert "Authorization" in headers
        assert "client_id" not in params

        method, creds = headers["Authorization"].split()
        assert method == "Basic"

        expected = f"{client_id}:{secret}"
        assert base64.b64decode(creds) == expected.encode("ascii")

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        check_client_authn(headers, params)

        if scope:
            assert params["scope"] == [scope]
        else:
            assert "scope" not in params

        resp = {
            "device_code": device_code,
            "user_code": user_code,
            "interval": 0,
            uri_spelling: verification_url,
            "expires_in": 5,
        }

        return 200, resp

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    attempts = 0
    retry_lock = threading.Lock()

    def token_endpoint(headers, params):
        check_client_authn(headers, params)

        assert params["grant_type"] == ["urn:ietf:params:oauth:grant-type:device_code"]
        assert params["device_code"] == [device_code]

        now = time.monotonic()

        with retry_lock:
            nonlocal attempts

            # If the test wants to force the client to retry, return an
            # authorization_pending response and decrement the retry count.
            if attempts < retries:
                attempts += 1
                return 400, {"error": "authorization_pending"}

        # Successfully finish the request by sending the access bearer token.
        resp = {
            "access_token": access_token,
            "token_type": "bearer",
        }

        return 200, resp

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    # First connection is a discovery request, which should result in the above
    # endpoints being called.
    with sock:
        handle_discovery_connection(sock, openid_provider.discovery_uri)

    # Client should reconnect.
    sock, _ = accept()
    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            initial = start_oauth_handshake(conn)

            # Validate and accept the token.
            auth = get_auth_value(initial)
            assert auth == f"Bearer {access_token}".encode("ascii")

            if success:
                finish_handshake(conn)

            elif abnormal_failure:
                # Send an empty error response, which should result in a
                # mechanism-level failure in the client. This test ensures that
                # the client doesn't try a third connection for this case.
                expected_error = "server sent error response without a status"
                fail_oauth_handshake(conn, {})

            else:
                # Simulate token validation failure.
                resp = {
                    "status": "invalid_token",
                    "openid-configuration": openid_provider.discovery_uri,
                }
                expected_error = "test token validation failure"
                fail_oauth_handshake(conn, resp, errmsg=expected_error)

    if retries:
        # Finally, make sure that the client prompted the user once with the
        # expected authorization URL and user code.
        assert len(auth_data_cb.calls) == 2

        # First call should have been for a custom flow, which we ignored.
        assert auth_data_cb.calls[0].type == PQAUTHDATA_OAUTH_BEARER_TOKEN

        # Second call is for our user prompt.
        call = auth_data_cb.calls[1]
        assert call.type == PQAUTHDATA_PROMPT_OAUTH_DEVICE
        assert call.verification_uri.decode() == verification_url
        assert call.user_code.decode() == user_code
        assert call.verification_uri_complete is None
        assert call.expires_in == 5

    if not success:
        # The client should not try to connect again.
        with pytest.raises(psycopg2.OperationalError, match=expected_error):
            client.check_completed()


@pytest.mark.parametrize(
    "server_discovery",
    [
        pytest.param(True, id="server discovery"),
        pytest.param(False, id="direct discovery"),
    ],
)
@pytest.mark.parametrize(
    "issuer, path",
    [
        pytest.param(
            "{issuer}",
            "/.well-known/oauth-authorization-server",
            id="oauth",
        ),
        pytest.param(
            "{issuer}/alt",
            "/.well-known/oauth-authorization-server/alt",
            id="oauth with path, IETF style",
        ),
        pytest.param(
            "{issuer}/alt",
            "/alt/.well-known/oauth-authorization-server",
            id="oauth with path, broken OIDC style",
        ),
        pytest.param(
            "{issuer}/alt",
            "/alt/.well-known/openid-configuration",
            id="openid with path, OIDC style",
        ),
        pytest.param(
            "{issuer}/alt",
            "/.well-known/openid-configuration/alt",
            id="openid with path, IETF style",
        ),
        pytest.param(
            "{issuer}/",
            "//.well-known/openid-configuration",
            id="empty path segment, OIDC style",
        ),
        pytest.param(
            "{issuer}/",
            "/.well-known/openid-configuration/",
            id="empty path segment, IETF style",
        ),
    ],
)
def test_alternate_well_known_paths(
    accept, openid_provider, issuer, path, server_discovery
):
    issuer = issuer.format(issuer=openid_provider.issuer)
    discovery_uri = openid_provider.issuer + path

    client_id = secrets.token_hex()
    access_token = secrets.token_urlsafe()

    def discovery_handler(*args):
        """
        Pass-through implementation of the discovery handler. Modifies the
        default document to contain this test's issuer identifier.
        """
        code, doc = openid_provider._default_discovery_handler(*args)
        doc["issuer"] = issuer
        return code, doc

    openid_provider.register_endpoint(None, "GET", path, discovery_handler)

    def authorization_endpoint(headers, params):
        resp = {
            "device_code": "12345",
            "user_code": "ABCDE",
            "interval": 0,
            "verification_url": "https://example.com/device",
            "expires_in": 5,
        }

        return 200, resp

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    def token_endpoint(headers, params):
        # Successfully finish the request by sending the access bearer token.
        resp = {
            "access_token": access_token,
            "token_type": "bearer",
        }

        return 200, resp

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    kwargs = dict(oauth_client_id=client_id)
    if server_discovery:
        kwargs.update(oauth_issuer=issuer)
    else:
        kwargs.update(oauth_issuer=discovery_uri)

    sock, client = accept(**kwargs)

    with sock:
        handle_discovery_connection(sock, discovery_uri)

    # Expect the client to connect again.
    sock, _ = accept()
    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            initial = start_oauth_handshake(conn)

            # Validate the token.
            auth = get_auth_value(initial)
            assert auth == f"Bearer {access_token}".encode("ascii")

            finish_handshake(conn)


@pytest.mark.parametrize(
    "server_discovery",
    [
        pytest.param(True, id="server discovery"),
        pytest.param(False, id="direct discovery"),
    ],
)
@pytest.mark.parametrize(
    "issuer, path, expected_error",
    [
        pytest.param(
            "{issuer}",
            "/.well-known/oauth-authorization-server/",
            None,
            id="extra empty segment (no path)",
        ),
        pytest.param(
            "{issuer}/path",
            "/.well-known/oauth-authorization-server/path/",
            None,
            id="extra empty segment (with path)",
        ),
        pytest.param(
            "{issuer}",
            "?/.well-known/oauth-authorization-server",
            r'OAuth discovery URI ".*" must not contain query or fragment components',
            id="query",
        ),
        pytest.param(
            "{issuer}",
            "#/.well-known/oauth-authorization-server",
            r'OAuth discovery URI ".*" must not contain query or fragment components',
            id="fragment",
        ),
        pytest.param(
            "{issuer}/sub/path",
            "/sub/.well-known/oauth-authorization-server/path",
            r'OAuth discovery URI ".*" uses an invalid format',
            id="sandwiched prefix",
        ),
        pytest.param(
            "{issuer}/path",
            "/path/openid-configuration",
            r'OAuth discovery URI ".*" is not a .well-known URI',
            id="not .well-known",
        ),
        pytest.param(
            "{issuer}",
            "https://.well-known/oauth-authorization-server",
            r'OAuth discovery URI ".*" is not a .well-known URI',
            id=".well-known prefix buried in the authority",
        ),
        pytest.param(
            "{issuer}",
            "/.well-known/oauth-protected-resource",
            r'OAuth discovery URI ".*" uses an unsupported .well-known suffix',
            id="unknown well-known suffix",
        ),
        pytest.param(
            "{issuer}/path",
            "/path/.well-known/openid-configuration-2",
            r'OAuth discovery URI ".*" uses an unsupported .well-known suffix',
            id="unknown well-known suffix, OIDC style",
        ),
        pytest.param(
            "{issuer}/path",
            "/.well-known/oauth-authorization-server-2/path",
            r'OAuth discovery URI ".*" uses an unsupported .well-known suffix',
            id="unknown well-known suffix, IETF style",
        ),
        pytest.param(
            "{issuer}",
            "file:///.well-known/oauth-authorization-server",
            r'OAuth discovery URI ".*" must use HTTPS',
            id="unsupported scheme",
        ),
    ],
)
def test_bad_well_known_paths(
    accept, openid_provider, issuer, path, expected_error, server_discovery
):
    if not server_discovery and "/.well-known/" not in path:
        # An oauth_issuer without a /.well-known/ path segment is just a normal
        # issuer identifier, so this isn't an interesting test.
        pytest.skip("not interesting: direct discovery requires .well-known")

    issuer = issuer.format(issuer=openid_provider.issuer)
    discovery_uri = urllib.parse.urljoin(openid_provider.issuer, path)

    client_id = secrets.token_hex()

    def discovery_handler(*args):
        """
        Pass-through implementation of the discovery handler. Modifies the
        default document to contain this test's issuer identifier.
        """
        code, doc = openid_provider._default_discovery_handler(*args)
        doc["issuer"] = issuer
        return code, doc

    openid_provider.register_endpoint(None, "GET", path, discovery_handler)

    def fail(*args):
        """
        No other endpoints should be contacted; fail if the client tries.
        """
        assert False, "endpoint unexpectedly called"

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", fail
    )
    openid_provider.register_endpoint("token_endpoint", "POST", "/token", fail)

    kwargs = dict(oauth_client_id=client_id)
    if server_discovery:
        kwargs.update(oauth_issuer=issuer)
    else:
        kwargs.update(oauth_issuer=discovery_uri)

    sock, client = accept(**kwargs)
    with sock:
        if expected_error and not server_discovery:
            # If the client already knows the URL, it should disconnect as soon
            # as it realizes it's not valid.
            expect_disconnected_handshake(sock)
        else:
            # Otherwise, it should complete the connection.
            handle_discovery_connection(sock, discovery_uri)

    # The client should not reconnect.

    if expected_error is None:
        if server_discovery:
            expected_error = rf"server's discovery document at {discovery_uri} \(issuer \".*\"\) is incompatible with oauth_issuer \({issuer}\)"
        else:
            expected_error = rf"the issuer identifier \({issuer}\) does not match oauth_issuer \(.*\)"

    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()


def expect_disconnected_handshake(sock):
    """
    Helper for any tests that expect the client to disconnect immediately after
    being sent the OAUTHBEARER SASL method. Generally speaking, this requires
    the client to have an oauth_issuer set so that it doesn't try to go through
    discovery.
    """
    with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
        # Initiate a handshake.
        startup = pq3.recv1(conn, cls=pq3.Startup)
        assert startup.proto == pq3.protocol(3, 0)

        pq3.send(
            conn,
            pq3.types.AuthnRequest,
            type=pq3.authn.SASL,
            body=[b"OAUTHBEARER", b""],
        )

        # The client should disconnect at this point.
        assert not conn.read(1), "client sent unexpected data"


@pytest.mark.parametrize(
    "missing",
    [
        pytest.param(["oauth_issuer"], id="missing oauth_issuer"),
        pytest.param(["oauth_client_id"], id="missing oauth_client_id"),
        pytest.param(["oauth_client_id", "oauth_issuer"], id="missing both"),
    ],
)
def test_oauth_requires_issuer_and_client_id(accept, openid_provider, missing):
    params = dict(
        oauth_issuer=openid_provider.issuer,
        oauth_client_id="some-id",
    )

    # Remove required parameters. This should cause a client error after the
    # server asks for OAUTHBEARER and the client tries to contact the issuer.
    for k in missing:
        del params[k]

    sock, client = accept(**params)
    with sock:
        expect_disconnected_handshake(sock)

    expected_error = "oauth_issuer and oauth_client_id are not both set"
    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()


# See https://datatracker.ietf.org/doc/html/rfc6749#appendix-A for character
# class definitions.
all_vschars = "".join([chr(c) for c in range(0x20, 0x7F)])
all_nqchars = "".join([chr(c) for c in range(0x21, 0x7F) if c not in (0x22, 0x5C)])


@pytest.mark.parametrize("client_id", ["", ":", " + ", r'+=&"\/~', all_vschars])
@pytest.mark.parametrize("secret", [None, "", ":", " + ", r'+=&"\/~', all_vschars])
@pytest.mark.parametrize("device_code", ["", " + ", r'+=&"\/~', all_vschars])
@pytest.mark.parametrize("scope", ["&", r"+=&/", all_nqchars])
def test_url_encoding(accept, openid_provider, client_id, secret, device_code, scope):
    sock, client = accept(
        oauth_issuer=openid_provider.discovery_uri,
        oauth_client_id=client_id,
        oauth_client_secret=secret,
        oauth_scope=scope,
    )

    user_code = f"{secrets.token_hex(2)}-{secrets.token_hex(2)}"
    verification_url = "https://example.com/device"

    access_token = secrets.token_urlsafe()

    def check_client_authn(headers, params):
        if secret is None:
            assert "Authorization" not in headers
            assert params["client_id"] == [client_id]
            return

        # Require the client to use Basic authn; request-body credentials are
        # NOT RECOMMENDED (RFC 6749, Sec. 2.3.1).
        assert "Authorization" in headers
        assert "client_id" not in params

        method, creds = headers["Authorization"].split()
        assert method == "Basic"

        decoded = base64.b64decode(creds).decode("utf-8")
        username, password = decoded.split(":", 1)

        expected_username = urllib.parse.quote_plus(client_id)
        expected_password = urllib.parse.quote_plus(secret)

        assert [username, password] == [expected_username, expected_password]

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        check_client_authn(headers, params)

        if scope:
            assert params["scope"] == [scope]
        else:
            assert "scope" not in params

        resp = {
            "device_code": device_code,
            "user_code": user_code,
            "interval": 0,
            "verification_url": verification_url,
            "expires_in": 5,
        }

        return 200, resp

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    def token_endpoint(headers, params):
        check_client_authn(headers, params)

        assert params["grant_type"] == ["urn:ietf:params:oauth:grant-type:device_code"]
        assert params["device_code"] == [device_code]

        # Successfully finish the request by sending the access bearer token.
        resp = {
            "access_token": access_token,
            "token_type": "bearer",
        }

        return 200, resp

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    # First connection is a discovery request, which should result in the above
    # endpoints being called.
    with sock:
        handle_discovery_connection(sock, openid_provider.discovery_uri)

    # Second connection sends the token.
    sock, _ = accept()
    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            initial = start_oauth_handshake(conn)

            # Validate and accept the token.
            auth = get_auth_value(initial)
            assert auth == f"Bearer {access_token}".encode("ascii")

            finish_handshake(conn)


@pytest.mark.slow
@pytest.mark.parametrize("error_code", ["authorization_pending", "slow_down"])
@pytest.mark.parametrize("retries", [1, 2])
@pytest.mark.parametrize("omit_interval", [True, False])
def test_oauth_retry_interval(
    accept, openid_provider, omit_interval, retries, error_code
):
    sock, client = accept(
        oauth_issuer=openid_provider.discovery_uri,
        oauth_client_id="some-id",
    )

    expected_retry_interval = 5 if omit_interval else 1
    access_token = secrets.token_urlsafe()

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        resp = {
            "device_code": "my-device-code",
            "user_code": "my-user-code",
            "verification_uri": "https://example.com",
            "expires_in": 5,
        }

        if not omit_interval:
            resp["interval"] = expected_retry_interval

        return 200, resp

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    attempts = 0
    last_retry = None
    retry_lock = threading.Lock()
    token_sent = threading.Event()

    def token_endpoint(headers, params):
        now = time.monotonic()

        with retry_lock:
            nonlocal attempts, last_retry, expected_retry_interval

            # Make sure the retry interval is being respected by the client.
            if last_retry is not None:
                interval = now - last_retry
                assert interval >= expected_retry_interval

            last_retry = now

            # If the test wants to force the client to retry, return the desired
            # error response and decrement the retry count.
            if attempts < retries:
                attempts += 1

                # A slow_down code requires the client to additionally increase
                # its interval by five seconds.
                if error_code == "slow_down":
                    expected_retry_interval += 5

                return 400, {"error": error_code}

        # Successfully finish the request by sending the access bearer token,
        # and signal the main thread to continue.
        resp = {
            "access_token": access_token,
            "token_type": "bearer",
        }
        token_sent.set()

        return 200, resp

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    # First connection is a discovery request, which should result in the above
    # endpoints being called.
    with sock:
        handle_discovery_connection(sock, openid_provider.discovery_uri)

    # At this point the client is talking to the authorization server. Wait for
    # that to succeed so we don't run into the accept() timeout.
    token_sent.wait()

    # Client should reconnect and send the token.
    sock, _ = accept()
    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            initial = start_oauth_handshake(conn)

            # Validate and accept the token.
            auth = get_auth_value(initial)
            assert auth == f"Bearer {access_token}".encode("ascii")

            finish_handshake(conn)


@pytest.fixture
def self_pipe():
    """
    Yields a pipe fd pair.
    """

    class _Pipe:
        pass

    p = _Pipe()
    p.readfd, p.writefd = os.pipe()

    try:
        yield p
    finally:
        os.close(p.readfd)
        os.close(p.writefd)


@pytest.mark.parametrize("scope", [None, "", "openid email"])
@pytest.mark.parametrize(
    "retries",
    [
        -1,  # no async callback
        0,  # async callback immediately returns token
        1,  # async callback waits on altsock once
        2,  # async callback waits on altsock twice
    ],
)
@pytest.mark.parametrize(
    "asynchronous",
    [
        pytest.param(False, id="synchronous"),
        pytest.param(True, id="asynchronous"),
    ],
)
def test_user_defined_flow(
    accept, auth_data_cb, self_pipe, scope, retries, asynchronous
):
    issuer = "http://localhost"
    discovery_uri = issuer + "/.well-known/openid-configuration"
    access_token = secrets.token_urlsafe()

    sock, client = accept(
        oauth_issuer=discovery_uri,
        oauth_client_id="some-id",
        oauth_scope=scope,
        async_=asynchronous,
    )

    # Track callbacks.
    attempts = 0
    wakeup_called = False
    cleanup_calls = 0
    lock = threading.Lock()

    def wakeup():
        """Writes a byte to the wakeup pipe."""
        nonlocal wakeup_called
        with lock:
            wakeup_called = True
            os.write(self_pipe.writefd, b"\0")

    def get_token(pgconn, request, p_altsock):
        """
        Async token callback. While attempts < retries, libpq will be instructed
        to wait on the self_pipe. When attempts == retries, the token will be
        set.

        Note that assertions and exceptions raised here are allowed but not very
        helpful, since they can't bubble through the libpq stack to be collected
        by the test suite. Try not to rely too heavily on them.
        """
        # Make sure libpq passed our user data through.
        assert request.user == 42

        with lock:
            nonlocal attempts, wakeup_called

            if attempts:
                # If we've already started the timer, we shouldn't get a
                # call back before it trips.
                assert wakeup_called, "authdata hook was called before the timer"

                # Drain the wakeup byte.
                os.read(self_pipe.readfd, 1)

            if attempts < retries:
                attempts += 1

                # Wake up the client in a little bit of time.
                wakeup_called = False
                threading.Timer(0.1, wakeup).start()

                # Tell libpq to wait on the other end of the wakeup pipe.
                p_altsock[0] = self_pipe.readfd
                return PGRES_POLLING_READING

        # Done!
        request.token = access_token.encode()
        return PGRES_POLLING_OK

    @ctypes.CFUNCTYPE(
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.POINTER(PGOAuthBearerRequest),
        ctypes.POINTER(ctypes.c_int),
    )
    def get_token_wrapper(pgconn, p_request, p_altsock):
        """
        Translation layer between C and Python for the async callback.
        Assertions and exceptions will be swallowed at the boundary, so make
        sure they don't escape here.
        """
        try:
            return get_token(pgconn, p_request.contents, p_altsock)
        except Exception:
            logging.error("Exception during async callback:\n" + traceback.format_exc())
            return PGRES_POLLING_FAILED

    @ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.POINTER(PGOAuthBearerRequest))
    def cleanup(pgconn, p_request):
        """
        Should be called exactly once per connection.
        """
        nonlocal cleanup_calls
        with lock:
            cleanup_calls += 1

    def bearer_hook(typ, pgconn, request):
        """
        Implementation of the PQAuthDataHook, which either sets up an async
        callback or returns the token directly, depending on the value of
        retries.

        As above, try not to rely too much on assertions/exceptions here.
        """
        assert typ == PQAUTHDATA_OAUTH_BEARER_TOKEN
        request.cleanup = cleanup

        if retries < 0:
            # Special case: return a token immediately without a callback.
            request.token = access_token.encode()
            return 1

        # Tell libpq to call us back.
        request.async_ = get_token_wrapper
        request.user = ctypes.c_void_p(42)  # will be checked in the callback
        return 1

    auth_data_cb.impl = bearer_hook

    # Now drive the server side.
    if retries >= 0:
        # First connection is a discovery request, which should result in the
        # hook being invoked.
        with sock:
            handle_discovery_connection(sock, discovery_uri)

        # Client should reconnect to send the token.
        sock, _ = accept()

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            # Initiate a handshake, which should result in our custom callback
            # being invoked to fetch the token.
            initial = start_oauth_handshake(conn)

            # Validate and accept the token.
            auth = get_auth_value(initial)
            assert auth == f"Bearer {access_token}".encode("ascii")

            finish_handshake(conn)

    # Check the data provided to the hook.
    assert len(auth_data_cb.calls) == 1

    call = auth_data_cb.calls[0]
    assert call.type == PQAUTHDATA_OAUTH_BEARER_TOKEN
    assert call.openid_configuration.decode() == discovery_uri
    assert call.scope == (None if scope is None else scope.encode())

    # Make sure we clean up after ourselves when the connection is finished.
    client.check_completed()
    assert cleanup_calls == 1


def alt_patterns(*patterns):
    """
    Just combines multiple alternative regexes into one. It's not very efficient
    but IMO it's easier to read and maintain.
    """
    pat = ""

    for p in patterns:
        if pat:
            pat += "|"
        pat += f"({p})"

    return pat


@pytest.mark.parametrize(
    "failure_mode, error_pattern",
    [
        pytest.param(
            (
                401,
                {
                    "error": "invalid_client",
                    "error_description": "client authentication failed",
                },
            ),
            r"failed to obtain device authorization: client authentication failed \(invalid_client\)",
            id="authentication failure with description",
        ),
        pytest.param(
            (400, {"error": "invalid_request"}),
            r"failed to obtain device authorization: \(invalid_request\)",
            id="invalid request without description",
        ),
        pytest.param(
            (400, {"error": "invalid_request", "padding": "x" * 256 * 1024}),
            r"failed to obtain device authorization: response is too large",
            id="gigantic authz response",
        ),
        pytest.param(
            (200, RawResponse('{"":' + "[" * 16)),
            r"failed to parse device authorization: JSON is too deeply nested",
            id="overly nested authz response array",
        ),
        pytest.param(
            (200, RawResponse('{"":' * 17)),
            r"failed to parse device authorization: JSON is too deeply nested",
            id="overly nested authz response object",
        ),
        pytest.param(
            (400, {}),
            r'failed to parse token error response: field "error" is missing',
            id="broken error response",
        ),
        pytest.param(
            (401, {"error": "invalid_client"}),
            r"failed to obtain device authorization: provider requires client authentication, and no oauth_client_secret is set \(invalid_client\)",
            id="failed authentication without description",
        ),
        pytest.param(
            (200, RawResponse(r'{ "interval": 3.5.8 }')),
            r"failed to parse device authorization: Token .* is invalid",
            id="non-numeric interval",
        ),
        pytest.param(
            (200, RawResponse(r'{ "interval": 08 }')),
            r"failed to parse device authorization: Token .* is invalid",
            id="invalid numeric interval",
        ),
    ],
)
def test_oauth_device_authorization_failures(
    accept, openid_provider, failure_mode, error_pattern
):
    client_id = secrets.token_hex()

    sock, client = accept(
        oauth_issuer=openid_provider.discovery_uri,
        oauth_client_id=client_id,
    )

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        return failure_mode

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    def token_endpoint(headers, params):
        assert False, "token endpoint was invoked unexpectedly"

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    with sock:
        handle_discovery_connection(sock, openid_provider.discovery_uri)

    # Now make sure the client correctly failed.
    with pytest.raises(psycopg2.OperationalError, match=error_pattern):
        client.check_completed()


Missing = object()  # sentinel for test_oauth_device_authorization_bad_json()


@pytest.mark.parametrize(
    "bad_value",
    [
        pytest.param({"device_code": 3}, id="object"),
        pytest.param([1, 2, 3], id="array"),
        pytest.param("some string", id="string"),
        pytest.param(4, id="numeric"),
        pytest.param(False, id="boolean"),
        pytest.param(None, id="null"),
        pytest.param(Missing, id="missing"),
    ],
)
@pytest.mark.parametrize(
    "field_name,ok_type,required",
    [
        ("device_code", str, True),
        ("user_code", str, True),
        ("verification_uri", str, True),
        ("interval", int, False),
    ],
)
def test_oauth_device_authorization_bad_json_schema(
    accept, openid_provider, field_name, ok_type, required, bad_value
):
    # To make the test matrix easy, just skip the tests that aren't actually
    # interesting (field of the correct type, missing optional field).
    if bad_value is Missing and not required:
        pytest.skip("not interesting: optional field")
    elif type(bad_value) == ok_type:  # not isinstance(), because bool is an int
        pytest.skip("not interesting: correct type")

    sock, client = accept(
        oauth_issuer=openid_provider.discovery_uri,
        oauth_client_id=secrets.token_hex(),
    )

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        # Begin with an acceptable base response...
        resp = {
            "device_code": "my-device-code",
            "user_code": "my-user-code",
            "interval": 0,
            "verification_uri": "https://example.com",
            "expires_in": 5,
        }

        # ...then tweak it so the client fails.
        if bad_value is Missing:
            del resp[field_name]
        else:
            resp[field_name] = bad_value

        return 200, resp

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    def token_endpoint(headers, params):
        assert False, "token endpoint was invoked unexpectedly"

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    with sock:
        handle_discovery_connection(sock, openid_provider.discovery_uri)

    # Now make sure the client correctly failed.
    if bad_value is Missing:
        error_pattern = f'field "{field_name}" is missing'
    elif ok_type == str:
        error_pattern = f'field "{field_name}" must be a string'
    elif ok_type == int:
        error_pattern = f'field "{field_name}" must be a number'
    else:
        assert False, "update error_pattern for new failure mode"

    with pytest.raises(psycopg2.OperationalError, match=error_pattern):
        client.check_completed()


@pytest.mark.parametrize(
    "failure_mode, error_pattern",
    [
        pytest.param(
            (
                400,
                {
                    "error": "expired_token",
                    "error_description": "the device code has expired",
                },
            ),
            r"failed to obtain access token: the device code has expired \(expired_token\)",
            id="expired token with description",
        ),
        pytest.param(
            (400, {"error": "access_denied"}),
            r"failed to obtain access token: \(access_denied\)",
            id="access denied without description",
        ),
        pytest.param(
            (400, {"error": "access_denied", "padding": "x" * 256 * 1024}),
            r"failed to obtain access token: response is too large",
            id="gigantic token response",
        ),
        pytest.param(
            (200, RawResponse('{"":' + "[" * 16)),
            r"failed to parse access token response: JSON is too deeply nested",
            id="overly nested token response array",
        ),
        pytest.param(
            (200, RawResponse('{"":' * 17)),
            r"failed to parse access token response: JSON is too deeply nested",
            id="overly nested token response object",
        ),
        pytest.param(
            (400, {}),
            r'failed to parse token error response: field "error" is missing',
            id="empty error response",
        ),
        pytest.param(
            (401, {"error": "invalid_client"}),
            r"failed to obtain access token: provider requires client authentication, and no oauth_client_secret is set \(invalid_client\)",
            id="authentication failure without description",
        ),
        pytest.param(
            (200, {}, {}),
            r"failed to parse access token response: no content type was provided",
            id="missing content type",
        ),
        pytest.param(
            (200, {"Content-Type": "text/plain"}, {}),
            r"failed to parse access token response: unexpected content type",
            id="wrong content type",
        ),
        pytest.param(
            (200, {"Content-Type": "application/jsonx"}, {}),
            r"failed to parse access token response: unexpected content type",
            id="wrong content type (correct prefix)",
        ),
    ],
)
@pytest.mark.parametrize("retries", [0, 1])
def test_oauth_token_failures(
    accept, openid_provider, retries, failure_mode, error_pattern
):
    client_id = secrets.token_hex()

    sock, client = accept(
        oauth_issuer=openid_provider.discovery_uri,
        oauth_client_id=client_id,
    )

    device_code = secrets.token_hex()
    user_code = f"{secrets.token_hex(2)}-{secrets.token_hex(2)}"

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        assert params["client_id"] == [client_id]

        resp = {
            "device_code": device_code,
            "user_code": user_code,
            "interval": 0,
            "verification_uri": "https://example.com/device",
            "expires_in": 5,
        }

        return 200, resp

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    retry_lock = threading.Lock()
    final_sent = False

    def token_endpoint(headers, params):
        with retry_lock:
            nonlocal retries, final_sent

            # If the test wants to force the client to retry, return an
            # authorization_pending response and decrement the retry count.
            if retries > 0:
                retries -= 1
                return 400, {"error": "authorization_pending"}

            # We should only return our failure_mode response once; any further
            # requests indicate that the client isn't correctly bailing out.
            assert not final_sent, "client continued after token error"

            final_sent = True

        return failure_mode

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    with sock:
        handle_discovery_connection(sock, openid_provider.discovery_uri)

    # Now make sure the client correctly failed.
    with pytest.raises(psycopg2.OperationalError, match=error_pattern):
        client.check_completed()


@pytest.mark.parametrize(
    "bad_value",
    [
        pytest.param({"device_code": 3}, id="object"),
        pytest.param([1, 2, 3], id="array"),
        pytest.param("some string", id="string"),
        pytest.param(4, id="numeric"),
        pytest.param(False, id="boolean"),
        pytest.param(None, id="null"),
        pytest.param(Missing, id="missing"),
    ],
)
@pytest.mark.parametrize(
    "field_name,ok_type,required",
    [
        ("access_token", str, True),
        ("token_type", str, True),
    ],
)
def test_oauth_token_bad_json_schema(
    accept, openid_provider, field_name, ok_type, required, bad_value
):
    # To make the test matrix easy, just skip the tests that aren't actually
    # interesting (field of the correct type, missing optional field).
    if bad_value is Missing and not required:
        pytest.skip("not interesting: optional field")
    elif type(bad_value) == ok_type:  # not isinstance(), because bool is an int
        pytest.skip("not interesting: correct type")

    sock, client = accept(
        oauth_issuer=openid_provider.discovery_uri,
        oauth_client_id=secrets.token_hex(),
    )

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        resp = {
            "device_code": "my-device-code",
            "user_code": "my-user-code",
            "interval": 0,
            "verification_uri": "https://example.com",
            "expires_in": 5,
        }

        return 200, resp

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    def token_endpoint(headers, params):
        # Begin with an acceptable base response...
        resp = {
            "access_token": secrets.token_urlsafe(),
            "token_type": "bearer",
        }

        # ...then tweak it so the client fails.
        if bad_value is Missing:
            del resp[field_name]
        else:
            resp[field_name] = bad_value

        return 200, resp

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    with sock:
        handle_discovery_connection(sock, openid_provider.discovery_uri)

    # Now make sure the client correctly failed.
    error_pattern = "failed to parse access token response: "
    if bad_value is Missing:
        error_pattern += f'field "{field_name}" is missing'
    elif ok_type == str:
        error_pattern += f'field "{field_name}" must be a string'
    elif ok_type == int:
        error_pattern += f'field "{field_name}" must be a number'
    else:
        assert False, "update error_pattern for new failure mode"

    with pytest.raises(psycopg2.OperationalError, match=error_pattern):
        client.check_completed()


@pytest.mark.parametrize("success", [True, False])
@pytest.mark.parametrize("scope", [None, "openid email"])
@pytest.mark.parametrize(
    "base_response",
    [
        {"status": "invalid_token"},
        {"extra_object": {"key": "value"}, "status": "invalid_token"},
        {"extra_object": {"status": 1}, "status": "invalid_token"},
    ],
)
def test_oauth_discovery(accept, openid_provider, base_response, scope, success):
    sock, client = accept(
        oauth_issuer=openid_provider.issuer,
        oauth_client_id=secrets.token_hex(),
    )

    device_code = secrets.token_hex()
    user_code = f"{secrets.token_hex(2)}-{secrets.token_hex(2)}"
    verification_url = "https://example.com/device"

    access_token = secrets.token_urlsafe()

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        if scope:
            assert params["scope"] == [scope]
        else:
            assert "scope" not in params

        resp = {
            "device_code": device_code,
            "user_code": user_code,
            "interval": 0,
            "verification_uri": verification_url,
            "expires_in": 5,
        }

        return 200, resp

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    def token_endpoint(headers, params):
        assert params["grant_type"] == ["urn:ietf:params:oauth:grant-type:device_code"]
        assert params["device_code"] == [device_code]

        # Successfully finish the request by sending the access bearer token.
        resp = {
            "access_token": access_token,
            "token_type": "bearer",
        }

        return 200, resp

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    # Construct the response to use when failing the SASL exchange. Return a
    # link to the discovery document, pointing to the test provider server.
    fail_resp = {
        **base_response,
        "openid-configuration": openid_provider.discovery_uri,
    }

    if scope:
        fail_resp["scope"] = scope

    with sock:
        handle_discovery_connection(sock, response=fail_resp)

    # The client will connect to us a second time, using the parameters we sent
    # it.
    sock, _ = accept()

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            initial = start_oauth_handshake(conn)

            # Validate the token.
            auth = get_auth_value(initial)
            assert auth == f"Bearer {access_token}".encode("ascii")

            if success:
                finish_handshake(conn)

            else:
                # Simulate token validation failure.
                expected_error = "test token validation failure"
                fail_oauth_handshake(conn, fail_resp, errmsg=expected_error)

    if not success:
        # The client should not try to connect again.
        with pytest.raises(psycopg2.OperationalError, match=expected_error):
            client.check_completed()


@pytest.mark.parametrize(
    "response,expected_error",
    [
        pytest.param(
            "abcde",
            'Token "abcde" is invalid',
            id="bad JSON: invalid syntax",
        ),
        pytest.param(
            b"\xff\xff\xff\xff",
            "server's error response is not valid UTF-8",
            id="bad JSON: invalid encoding",
        ),
        pytest.param(
            '"abcde"',
            "top-level element must be an object",
            id="bad JSON: top-level element is a string",
        ),
        pytest.param(
            "[]",
            "top-level element must be an object",
            id="bad JSON: top-level element is an array",
        ),
        pytest.param(
            "{}",
            "server sent error response without a status",
            id="bad JSON: no status member",
        ),
        pytest.param(
            '{ "status": null }',
            'field "status" must be a string',
            id="bad JSON: null status member",
        ),
        pytest.param(
            '{ "status": 0 }',
            'field "status" must be a string',
            id="bad JSON: int status member",
        ),
        pytest.param(
            '{ "status": [ "bad" ] }',
            'field "status" must be a string',
            id="bad JSON: array status member",
        ),
        pytest.param(
            '{ "status": { "bad": "bad" } }',
            'field "status" must be a string',
            id="bad JSON: object status member",
        ),
        pytest.param(
            '{ "nested": { "status": "bad" } }',
            "server sent error response without a status",
            id="bad JSON: nested status",
        ),
        pytest.param(
            '{ "status": "invalid_token" ',
            "The input string ended unexpectedly",
            id="bad JSON: unterminated object",
        ),
        pytest.param(
            '{ "status": "invalid_token" } { }',
            'Expected end of input, but found "{"',
            id="bad JSON: trailing data",
        ),
        pytest.param(
            '{ "status": "invalid_token", "openid-configuration": 1 }',
            'field "openid-configuration" must be a string',
            id="bad JSON: int openid-configuration member",
        ),
        pytest.param(
            '{ "status": "invalid_token", "openid-configuration": 1 }',
            'field "openid-configuration" must be a string',
            id="bad JSON: int openid-configuration member",
        ),
        pytest.param(
            '{ "status": "invalid_token", "openid-configuration": "", "openid-configuration": "" }',
            'field "openid-configuration" is duplicated',
            id="bad JSON: duplicated field",
        ),
        pytest.param(
            '{ "status": "invalid_token", "scope": 1 }',
            'field "scope" must be a string',
            id="bad JSON: int scope member",
        ),
    ],
)
def test_oauth_discovery_server_error(accept, response, expected_error):
    sock, client = accept(
        oauth_issuer="https://example.com",
        oauth_client_id=secrets.token_hex(),
    )

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            initial = start_oauth_handshake(conn)

            if isinstance(response, str):
                response = response.encode("utf-8")

            # Fail the SASL exchange with an invalid JSON response.
            pq3.send(
                conn,
                pq3.types.AuthnRequest,
                type=pq3.authn.SASLContinue,
                body=response,
            )

            # The client should disconnect, so the socket is closed here. (If
            # the client doesn't disconnect, it will report a different error
            # below and the test will fail.)

    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()


# All of these tests are expected to fail before libpq tries to actually attempt
# a connection to any endpoint. To avoid hitting the network in the event that a
# test fails, an invalid IPv4 address (256.256.256.256) is used as a hostname.
@pytest.mark.parametrize(
    "bad_response,expected_error",
    [
        pytest.param(
            (200, {"Content-Type": "text/plain"}, {}),
            r'failed to parse OpenID discovery document: unexpected content type: "text/plain"',
            id="not JSON",
        ),
        pytest.param(
            (200, {}, {}),
            r"failed to parse OpenID discovery document: no content type was provided",
            id="no Content-Type",
        ),
        pytest.param(
            (204, {}, None),
            r"failed to fetch OpenID discovery document: unexpected response code 204",
            id="no content",
        ),
        pytest.param(
            (301, {"Location": "https://localhost/"}, None),
            r"failed to fetch OpenID discovery document: unexpected response code 301",
            id="redirection",
        ),
        pytest.param(
            (404, {}),
            r"failed to fetch OpenID discovery document: unexpected response code 404",
            id="not found",
        ),
        pytest.param(
            (200, RawResponse("blah\x00blah")),
            r"failed to parse OpenID discovery document: response contains embedded NULLs",
            id="NULL bytes in document",
        ),
        pytest.param(
            (200, RawBytes(b"blah\xffblah")),
            r"failed to parse OpenID discovery document: response is not valid UTF-8",
            id="document is not UTF-8",
        ),
        pytest.param(
            (200, 123),
            r"failed to parse OpenID discovery document: top-level element must be an object",
            id="scalar at top level",
        ),
        pytest.param(
            (200, []),
            r"failed to parse OpenID discovery document: top-level element must be an object",
            id="array at top level",
        ),
        pytest.param(
            (200, RawResponse("{")),
            r"failed to parse OpenID discovery document.* input string ended unexpectedly",
            id="unclosed object",
        ),
        pytest.param(
            (200, RawResponse(r'{ "hello": ] }')),
            r"failed to parse OpenID discovery document.* Expected JSON value",
            id="bad array",
        ),
        pytest.param(
            (200, {"issuer": 123}),
            r'failed to parse OpenID discovery document: field "issuer" must be a string',
            id="non-string issuer",
        ),
        pytest.param(
            (200, {"issuer": ["something"]}),
            r'failed to parse OpenID discovery document: field "issuer" must be a string',
            id="issuer array",
        ),
        pytest.param(
            (200, {"issuer": {}}),
            r'failed to parse OpenID discovery document: field "issuer" must be a string',
            id="issuer object",
        ),
        pytest.param(
            (200, {"grant_types_supported": 123}),
            r'failed to parse OpenID discovery document: field "grant_types_supported" must be an array of strings',
            id="numeric grant types field",
        ),
        pytest.param(
            (
                200,
                {
                    "grant_types_supported": "urn:ietf:params:oauth:grant-type:device_code"
                },
            ),
            r'failed to parse OpenID discovery document: field "grant_types_supported" must be an array of strings',
            id="string grant types field",
        ),
        pytest.param(
            (200, {"grant_types_supported": {}}),
            r'failed to parse OpenID discovery document: field "grant_types_supported" must be an array of strings',
            id="object grant types field",
        ),
        pytest.param(
            (200, {"grant_types_supported": [123]}),
            r'failed to parse OpenID discovery document: field "grant_types_supported" must be an array of strings',
            id="non-string grant types",
        ),
        pytest.param(
            (200, {"grant_types_supported": ["something", 123]}),
            r'failed to parse OpenID discovery document: field "grant_types_supported" must be an array of strings',
            id="non-string grant types later in the list",
        ),
        pytest.param(
            (200, {"grant_types_supported": ["something", {}]}),
            r'failed to parse OpenID discovery document: field "grant_types_supported" must be an array of strings',
            id="object grant types later in the list",
        ),
        pytest.param(
            (200, {"grant_types_supported": ["something", ["something"]]}),
            r'failed to parse OpenID discovery document: field "grant_types_supported" must be an array of strings',
            id="embedded array grant types later in the list",
        ),
        pytest.param(
            (
                200,
                {
                    "grant_types_supported": ["something"],
                    "token_endpoint": "https://256.256.256.256/",
                    "issuer": 123,
                },
            ),
            r'failed to parse OpenID discovery document: field "issuer" must be a string',
            id="non-string issuer after other valid fields",
        ),
        pytest.param(
            (
                200,
                {
                    "ignored": {"grant_types_supported": 123, "token_endpoint": 123},
                    "issuer": 123,
                },
            ),
            r'failed to parse OpenID discovery document: field "issuer" must be a string',
            id="non-string issuer after other ignored fields",
        ),
        pytest.param(
            (200, {"token_endpoint": "https://256.256.256.256/"}),
            r'failed to parse OpenID discovery document: field "issuer" is missing',
            id="missing issuer",
        ),
        pytest.param(
            (200, {"issuer": "{issuer}"}),
            r'failed to parse OpenID discovery document: field "token_endpoint" is missing',
            id="missing token endpoint",
        ),
        pytest.param(
            (
                200,
                {
                    "issuer": "{issuer}",
                    "token_endpoint": "https://256.256.256.256/token",
                    "grant_types_supported": [
                        "urn:ietf:params:oauth:grant-type:device_code"
                    ],
                },
            ),
            r'cannot run OAuth device authorization: issuer "https://.*" does not provide a device authorization endpoint',
            id="missing device_authorization_endpoint",
        ),
        pytest.param(
            (
                200,
                {
                    "issuer": "{issuer}",
                    "token_endpoint": "https://256.256.256.256/token",
                    "grant_types_supported": [
                        "urn:ietf:params:oauth:grant-type:device_code"
                    ],
                    "device_authorization_endpoint": "https://256.256.256.256/dev",
                    "filler": "x" * 256 * 1024,
                },
            ),
            r"failed to fetch OpenID discovery document: response is too large",
            id="gigantic discovery response",
        ),
        pytest.param(
            (
                200,
                RawResponse('{"":' + "[" * 16),
            ),
            r"failed to parse OpenID discovery document: JSON is too deeply nested",
            id="overly nested discovery response array",
        ),
        pytest.param(
            (
                200,
                RawResponse('{"":' * 17),
            ),
            r"failed to parse OpenID discovery document: JSON is too deeply nested",
            id="overly nested discovery response object",
        ),
        pytest.param(
            (
                200,
                {
                    "issuer": "{issuer}/path",
                    "token_endpoint": "https://256.256.256.256/token",
                    "grant_types_supported": [
                        "urn:ietf:params:oauth:grant-type:device_code"
                    ],
                    "device_authorization_endpoint": "https://256.256.256.256/dev",
                },
            ),
            r"failed to parse OpenID discovery document: the issuer identifier \(https://.*/path\) does not match oauth_issuer \(https://.*\)",
            id="mismatched issuer identifier",
        ),
        pytest.param(
            (
                200,
                RawResponse(
                    """{
                        "issuer": "https://256.256.256.256/path",
                        "token_endpoint": "https://256.256.256.256/token",
                        "grant_types_supported": [
                            "urn:ietf:params:oauth:grant-type:device_code"
                        ],
                        "device_authorization_endpoint": "https://256.256.256.256/dev",
                        "device_authorization_endpoint": "https://256.256.256.256/dev"
                    }"""
                ),
            ),
            r'failed to parse OpenID discovery document: field "device_authorization_endpoint" is duplicated',
            id="duplicated field",
        ),
        #
        # Exercise HTTP-level failures by breaking the protocol. Note that the
        # error messages here are implementation-dependent.
        #
        pytest.param(
            (1000, {}),
            r"failed to fetch OpenID discovery document: Unsupported protocol \(.*\)",
            id="invalid HTTP response code",
        ),
        pytest.param(
            (200, {"Content-Length": -1}, {}),
            r"failed to fetch OpenID discovery document: Weird server reply \(.*Content-Length.*\)",
            id="bad HTTP Content-Length",
        ),
    ],
)
def test_oauth_discovery_provider_failure(
    accept, openid_provider, bad_response, expected_error
):
    sock, client = accept(
        oauth_issuer=openid_provider.discovery_uri,
        oauth_client_id=secrets.token_hex(),
    )

    def failing_discovery_handler(headers, params):
        try:
            # Insert the correct issuer value if the test wants to.
            resp = bad_response[1]
            iss = resp["issuer"]
            resp["issuer"] = iss.format(issuer=openid_provider.issuer)
        except (AttributeError, KeyError, TypeError):
            pass

        return bad_response

    openid_provider.register_endpoint(
        None,
        "GET",
        "/.well-known/openid-configuration",
        failing_discovery_handler,
    )

    with sock:
        handle_discovery_connection(sock, openid_provider.discovery_uri)

    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()


@pytest.mark.parametrize(
    "sasl_err,resp_type,resp_payload,expected_error",
    [
        pytest.param(
            {"status": "invalid_request"},
            pq3.types.ErrorResponse,
            dict(
                fields=[b"SFATAL", b"C28000", b"Mexpected error message", b""],
            ),
            "server rejected OAuth bearer token: invalid_request",
            id="standard server error: invalid_request",
        ),
        pytest.param(
            {"": [[[[[[[]]]]]]], "status": "invalid_request"},
            pq3.types.ErrorResponse,
            dict(
                fields=[b"SFATAL", b"C28000", b"Mexpected error message", b""],
            ),
            "server rejected OAuth bearer token: invalid_request",
            id="standard server error: invalid_request with ignored array",
        ),
        pytest.param(
            {"status": "invalid_token"},
            pq3.types.ErrorResponse,
            dict(
                fields=[b"SFATAL", b"C28000", b"Mexpected error message", b""],
            ),
            "expected error message",
            id="standard server error: invalid_token without discovery URI",
        ),
        pytest.param(
            {"status": "invalid_token", "openid-configuration": ""},
            pq3.types.AuthnRequest,
            dict(type=pq3.authn.SASLContinue, body=b""),
            "server sent additional OAuth data",
            id="broken server: additional challenge after error",
        ),
        pytest.param(
            {"status": "invalid_token", "openid-configuration": ""},
            pq3.types.AuthnRequest,
            dict(type=pq3.authn.SASLFinal),
            "server sent additional OAuth data",
            id="broken server: SASL success after error",
        ),
        pytest.param(
            {"status": "invalid_token", "openid-configuration": ""},
            pq3.types.AuthnRequest,
            dict(type=pq3.authn.SASL, body=[b"OAUTHBEARER", b""]),
            "duplicate SASL authentication request",
            id="broken server: SASL reinitialization after error",
        ),
        pytest.param(
            RawResponse('{"":' + "[" * 8),
            pq3.types.AuthnRequest,
            dict(type=pq3.authn.SASL, body=[b"OAUTHBEARER", b""]),
            "JSON is too deeply nested",
            id="broken server: overly nested JSON response array",
        ),
        pytest.param(
            RawResponse('{"":' * 9),
            pq3.types.AuthnRequest,
            dict(type=pq3.authn.SASL, body=[b"OAUTHBEARER", b""]),
            "JSON is too deeply nested",
            id="broken server: overly nested JSON response object",
        ),
    ],
)
def test_oauth_server_error(
    accept, auth_data_cb, sasl_err, resp_type, resp_payload, expected_error
):
    wkuri = f"https://256.256.256.256/.well-known/openid-configuration"
    sock, client = accept(
        oauth_issuer=wkuri,
        oauth_client_id="some-id",
    )

    def bearer_hook(typ, pgconn, request):
        """
        Implementation of the PQAuthDataHook, which returns a token directly so
        we don't need an openid_provider instance.
        """
        assert typ == PQAUTHDATA_OAUTH_BEARER_TOKEN
        request.token = secrets.token_urlsafe().encode()
        return 1

    auth_data_cb.impl = bearer_hook

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            start_oauth_handshake(conn)

            # Ignore the client data. Return an error "challenge".
            if isinstance(sasl_err, RawResponse):
                resp = sasl_err
            else:
                if "openid-configuration" in sasl_err:
                    sasl_err["openid-configuration"] = wkuri

                resp = json.dumps(sasl_err)

            resp = resp.encode("utf-8")
            pq3.send(
                conn, pq3.types.AuthnRequest, type=pq3.authn.SASLContinue, body=resp
            )

            # Per RFC, the client is required to send a dummy ^A response.
            pkt = pq3.recv1(conn)
            assert pkt.type == pq3.types.PasswordMessage
            assert pkt.payload == b"\x01"

            # Now fail the SASL exchange (in either a valid way, or an
            # invalid one, depending on the test).
            pq3.send(conn, resp_type, **resp_payload)

    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()


def test_oauth_interval_overflow(accept, openid_provider):
    """
    A really badly behaved server could send a huge interval and then
    immediately tell us to slow_down; ensure we handle this without breaking.
    """
    # (should be equivalent to the INT_MAX in limits.h)
    int_max = ctypes.c_uint(-1).value // 2

    sock, client = accept(
        oauth_issuer=openid_provider.discovery_uri,
        oauth_client_id=secrets.token_hex(),
    )

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        resp = {
            "device_code": "my-device-code",
            "user_code": "my-user-code",
            "verification_uri": "https://example.com",
            "expires_in": 5,
            "interval": int_max,
        }

        return 200, resp

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    def token_endpoint(headers, params):
        return 400, {"error": "slow_down"}

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    with sock:
        handle_discovery_connection(sock, openid_provider.discovery_uri)

    expected_error = "slow_down interval overflow"
    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()


def test_oauth_refuses_http(accept, openid_provider, monkeypatch):
    """
    HTTP must be refused without PGOAUTHDEBUG.
    """
    monkeypatch.delenv("PGOAUTHDEBUG")

    def to_http(uri):
        """Swaps out a URI's scheme for http."""
        parts = urllib.parse.urlparse(uri)
        parts = parts._replace(scheme="http")
        return urllib.parse.urlunparse(parts)

    sock, client = accept(
        oauth_issuer=to_http(openid_provider.issuer),
        oauth_client_id=secrets.token_hex(),
    )

    # No provider callbacks necessary; we should fail immediately.

    with sock:
        handle_discovery_connection(sock, to_http(openid_provider.discovery_uri))

    expected_error = r'OAuth discovery URI ".*" must use HTTPS'
    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()


@pytest.mark.parametrize("auth_type", [pq3.authn.OK, pq3.authn.SASLFinal])
def test_discovery_incorrectly_permits_connection(accept, auth_type):
    """
    Incorrectly responds to a client's discovery request with AuthenticationOK
    or AuthenticationSASLFinal. require_auth=oauth should catch the former, and
    the mechanism itself should catch the latter.
    """
    issuer = "https://256.256.256.256"
    sock, client = accept(
        oauth_issuer=issuer,
        oauth_client_id=secrets.token_hex(),
        require_auth="oauth",
    )

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            initial = start_oauth_handshake(conn)

            auth = get_auth_value(initial)
            assert auth == b""

            # Incorrectly log the client in. It should immediately disconnect.
            pq3.send(conn, pq3.types.AuthnRequest, type=auth_type)
            assert not conn.read(1), "client sent unexpected data"

    if auth_type == pq3.authn.OK:
        expected_error = "server did not complete authentication"
    else:
        expected_error = "server sent unexpected additional OAuth data"

    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()


def test_no_discovery_url_provided(accept):
    """
    Tests what happens when the client doesn't know who to contact and the
    server doesn't tell it.
    """
    issuer = "https://256.256.256.256"
    sock, client = accept(
        oauth_issuer=issuer,
        oauth_client_id=secrets.token_hex(),
    )

    with sock:
        handle_discovery_connection(sock, discovery=None)

    expected_error = "no discovery metadata was provided"
    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()


@pytest.mark.parametrize("change_between_connections", [False, True])
def test_discovery_url_changes(accept, openid_provider, change_between_connections):
    """
    Ensures that the client complains if the server agrees on the issuer, but
    disagrees on the discovery URL to be used.
    """

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        resp = {
            "device_code": "DEV",
            "user_code": "USER",
            "interval": 0,
            "verification_uri": "https://example.org",
            "expires_in": 5,
        }

        return 200, resp

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    def token_endpoint(headers, params):
        resp = {
            "access_token": secrets.token_urlsafe(),
            "token_type": "bearer",
        }

        return 200, resp

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    # Have the client connect.
    sock, client = accept(
        oauth_issuer=openid_provider.discovery_uri,
        oauth_client_id="some-id",
    )

    other_wkuri = f"{openid_provider.issuer}/.well-known/oauth-authorization-server"

    if not change_between_connections:
        # Immediately respond with the wrong URL.
        with sock:
            handle_discovery_connection(sock, other_wkuri)

    else:
        # First connection; use the right URL to begin with.
        with sock:
            handle_discovery_connection(sock, openid_provider.discovery_uri)

        # Second connection. Reject the token and switch the URL.
        sock, _ = accept()
        with sock:
            with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
                initial = start_oauth_handshake(conn)
                get_auth_value(initial)

                # Ignore the token; fail with a different discovery URL.
                resp = {
                    "status": "invalid_token",
                    "openid-configuration": other_wkuri,
                }
                fail_oauth_handshake(conn, resp)

    expected_error = rf"server's discovery document has moved to {other_wkuri} \(previous location was {openid_provider.discovery_uri}\)"
    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()
