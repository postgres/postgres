#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#

import base64
import http.server
import json
import secrets
import sys
import threading
import time
import urllib.parse

import psycopg2
import pytest

import pq3

from .conftest import BLOCKING_TIMEOUT


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


def xtest_oauth_success(conn):  # TODO
    initial = start_oauth_handshake(conn)

    auth = get_auth_value(initial)
    assert auth.startswith(b"Bearer ")

    # Accept the token. TODO actually validate
    pq3.send(conn, pq3.types.AuthnRequest, type=pq3.authn.SASLFinal)
    finish_handshake(conn)


class OpenIDProvider(threading.Thread):
    """
    A thread that runs a mock OpenID provider server.
    """

    def __init__(self, *, port):
        super().__init__()

        self.exception = None

        addr = ("", port)
        self.server = self._Server(addr, self._Handler)

        # TODO: allow HTTPS only, somehow
        oauth = self._OAuthState()
        oauth.host = f"localhost:{port}"
        oauth.issuer = f"http://localhost:{port}"

        # The following endpoints are required to be advertised by providers,
        # even though our chosen client implementation does not actually make
        # use of them.
        oauth.register_endpoint(
            "authorization_endpoint", "POST", "/authorize", self._authorization_handler
        )
        oauth.register_endpoint("jwks_uri", "GET", "/keys", self._jwks_handler)

        self.server.oauth = oauth

    def run(self):
        try:
            self.server.serve_forever()
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

        def register_endpoint(self, name, method, path, func):
            if method not in self._endpoints:
                self._endpoints[method] = {}

            self._endpoints[method][path] = func
            self.endpoint_paths[name] = path

        def endpoint(self, method, path):
            if method not in self._endpoints:
                return None

            return self._endpoints[method].get(path)

    class _Server(http.server.HTTPServer):
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

        def _discovery_handler(self, headers, params):
            oauth = self.server.oauth

            doc = {
                "issuer": oauth.issuer,
                "response_types_supported": ["token"],
                "subject_types_supported": ["public"],
                "id_token_signing_alg_values_supported": ["RS256"],
            }

            for name, path in oauth.endpoint_paths.items():
                doc[name] = oauth.issuer + path

            return 200, doc

        def _handle(self, *, params=None, handler=None):
            oauth = self.server.oauth
            assert self.headers["Host"] == oauth.host

            if handler is None:
                handler = oauth.endpoint(self.command, self.path)
                assert (
                    handler is not None
                ), f"no registered endpoint for {self.command} {self.path}"

            code, resp = handler(self.headers, params)

            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.end_headers()

            resp = json.dumps(resp)
            resp = resp.encode("utf-8")
            self.wfile.write(resp)

            self.close_connection = True

        def do_GET(self):
            if self.path == "/.well-known/openid-configuration":
                self._handle(handler=self._discovery_handler)
                return

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
            params = urllib.parse.parse_qs(body)

            self._handle(params=params)


@pytest.fixture
def openid_provider(unused_tcp_port_factory):
    """
    A fixture that returns the OAuth state of a running OpenID provider server. The
    server will be stopped when the fixture is torn down.
    """
    thread = OpenIDProvider(port=unused_tcp_port_factory())
    thread.start()

    try:
        yield thread.server.oauth
    finally:
        thread.stop()


@pytest.mark.parametrize("secret", [None, "", "hunter2"])
@pytest.mark.parametrize("scope", [None, "", "openid email"])
@pytest.mark.parametrize("retries", [0, 1])
def test_oauth_with_explicit_issuer(
    capfd, accept, openid_provider, retries, scope, secret
):
    client_id = secrets.token_hex()

    sock, client = accept(
        oauth_issuer=openid_provider.issuer,
        oauth_client_id=client_id,
        oauth_client_secret=secret,
        oauth_scope=scope,
    )

    device_code = secrets.token_hex()
    user_code = f"{secrets.token_hex(2)}-{secrets.token_hex(2)}"
    verification_url = "https://example.com/device"

    access_token = secrets.token_urlsafe()

    def check_client_authn(headers, params):
        if not secret:
            assert params["client_id"] == [client_id]
            return

        # Require the client to use Basic authn; request-body credentials are
        # NOT RECOMMENDED (RFC 6749, Sec. 2.3.1).
        assert "Authorization" in headers

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
            "verification_uri": verification_url,
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

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            # Initiate a handshake, which should result in the above endpoints
            # being called.
            initial = start_oauth_handshake(conn)

            # Validate and accept the token.
            auth = get_auth_value(initial)
            assert auth == f"Bearer {access_token}".encode("ascii")

            pq3.send(conn, pq3.types.AuthnRequest, type=pq3.authn.SASLFinal)
            finish_handshake(conn)

    if retries:
        # Finally, make sure that the client prompted the user with the expected
        # authorization URL and user code.
        expected = f"Visit {verification_url} and enter the code: {user_code}"
        _, stderr = capfd.readouterr()
        assert expected in stderr


def test_oauth_requires_client_id(accept, openid_provider):
    sock, client = accept(
        oauth_issuer=openid_provider.issuer,
        # Do not set a client ID; this should cause a client error after the
        # server asks for OAUTHBEARER and the client tries to contact the
        # issuer.
    )

    with sock:
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
            assert not conn.read()

    expected_error = "no oauth_client_id is set"
    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()


@pytest.mark.slow
@pytest.mark.parametrize("error_code", ["authorization_pending", "slow_down"])
@pytest.mark.parametrize("retries", [1, 2])
def test_oauth_retry_interval(accept, openid_provider, retries, error_code):
    sock, client = accept(
        oauth_issuer=openid_provider.issuer,
        oauth_client_id="some-id",
    )

    expected_retry_interval = 1
    access_token = secrets.token_urlsafe()

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        resp = {
            "device_code": "my-device-code",
            "user_code": "my-user-code",
            "interval": expected_retry_interval,
            "verification_uri": "https://example.com",
            "expires_in": 5,
        }

        return 200, resp

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    attempts = 0
    last_retry = None
    retry_lock = threading.Lock()

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

        # Successfully finish the request by sending the access bearer token.
        resp = {
            "access_token": access_token,
            "token_type": "bearer",
        }

        return 200, resp

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            # Initiate a handshake, which should result in the above endpoints
            # being called.
            initial = start_oauth_handshake(conn)

            # Validate and accept the token.
            auth = get_auth_value(initial)
            assert auth == f"Bearer {access_token}".encode("ascii")

            pq3.send(conn, pq3.types.AuthnRequest, type=pq3.authn.SASLFinal)
            finish_handshake(conn)


@pytest.mark.parametrize(
    "failure_mode, error_pattern",
    [
        pytest.param(
            {
                "error": "invalid_client",
                "error_description": "client authentication failed",
            },
            r"client authentication failed \(invalid_client\)",
            id="authentication failure with description",
        ),
        pytest.param(
            {"error": "invalid_request"},
            r"\(invalid_request\)",
            id="invalid request without description",
        ),
        pytest.param(
            {},
            r"failed to obtain device authorization",
            id="broken error response",
        ),
    ],
)
def test_oauth_device_authorization_failures(
    accept, openid_provider, failure_mode, error_pattern
):
    client_id = secrets.token_hex()

    sock, client = accept(
        oauth_issuer=openid_provider.issuer,
        oauth_client_id=client_id,
    )

    # Set up our provider callbacks.
    # NOTE that these callbacks will be called on a background thread. Don't do
    # any unprotected state mutation here.

    def authorization_endpoint(headers, params):
        return 400, failure_mode

    openid_provider.register_endpoint(
        "device_authorization_endpoint", "POST", "/device", authorization_endpoint
    )

    def token_endpoint(headers, params):
        assert False, "token endpoint was invoked unexpectedly"

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            # Initiate a handshake, which should result in the above endpoints
            # being called.
            startup = pq3.recv1(conn, cls=pq3.Startup)
            assert startup.proto == pq3.protocol(3, 0)

            pq3.send(
                conn,
                pq3.types.AuthnRequest,
                type=pq3.authn.SASL,
                body=[b"OAUTHBEARER", b""],
            )

            # The client should not continue the connection due to the hardcoded
            # provider failure; we disconnect here.

    # Now make sure the client correctly failed.
    with pytest.raises(psycopg2.OperationalError, match=error_pattern):
        client.check_completed()


@pytest.mark.parametrize(
    "failure_mode, error_pattern",
    [
        pytest.param(
            {
                "error": "expired_token",
                "error_description": "the device code has expired",
            },
            r"the device code has expired \(expired_token\)",
            id="expired token with description",
        ),
        pytest.param(
            {"error": "access_denied"},
            r"\(access_denied\)",
            id="access denied without description",
        ),
        pytest.param(
            {},
            r"OAuth token retrieval failed",
            id="broken error response",
        ),
    ],
)
@pytest.mark.parametrize("retries", [0, 1])
def test_oauth_token_failures(
    accept, openid_provider, retries, failure_mode, error_pattern
):
    client_id = secrets.token_hex()

    sock, client = accept(
        oauth_issuer=openid_provider.issuer,
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

    def token_endpoint(headers, params):
        with retry_lock:
            nonlocal retries

            # If the test wants to force the client to retry, return an
            # authorization_pending response and decrement the retry count.
            if retries > 0:
                retries -= 1
                return 400, {"error": "authorization_pending"}

        return 400, failure_mode

    openid_provider.register_endpoint(
        "token_endpoint", "POST", "/token", token_endpoint
    )

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            # Initiate a handshake, which should result in the above endpoints
            # being called.
            startup = pq3.recv1(conn, cls=pq3.Startup)
            assert startup.proto == pq3.protocol(3, 0)

            pq3.send(
                conn,
                pq3.types.AuthnRequest,
                type=pq3.authn.SASL,
                body=[b"OAUTHBEARER", b""],
            )

            # The client should not continue the connection due to the hardcoded
            # provider failure; we disconnect here.

    # Now make sure the client correctly failed.
    with pytest.raises(psycopg2.OperationalError, match=error_pattern):
        client.check_completed()


@pytest.mark.parametrize("scope", [None, "openid email"])
@pytest.mark.parametrize(
    "base_response",
    [
        {"status": "invalid_token"},
        {"extra_object": {"key": "value"}, "status": "invalid_token"},
        {"extra_object": {"status": 1}, "status": "invalid_token"},
    ],
)
def test_oauth_discovery(accept, openid_provider, base_response, scope):
    sock, client = accept(oauth_client_id=secrets.token_hex())

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

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            initial = start_oauth_handshake(conn)

            # For discovery, the client should send an empty auth header. See
            # RFC 7628, Sec. 4.3.
            auth = get_auth_value(initial)
            assert auth == b""

            # We will fail the first SASL exchange. First return a link to the
            # discovery document, pointing to the test provider server.
            resp = dict(base_response)

            discovery_uri = f"{openid_provider.issuer}/.well-known/openid-configuration"
            resp["openid-configuration"] = discovery_uri

            if scope:
                resp["scope"] = scope

            resp = json.dumps(resp)

            pq3.send(
                conn,
                pq3.types.AuthnRequest,
                type=pq3.authn.SASLContinue,
                body=resp.encode("ascii"),
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
                    b"Mdoesn't matter",
                    b"",
                ],
            )

    # The client will connect to us a second time, using the parameters we sent
    # it.
    sock, _ = accept()

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            initial = start_oauth_handshake(conn)

            # Validate and accept the token.
            auth = get_auth_value(initial)
            assert auth == f"Bearer {access_token}".encode("ascii")

            pq3.send(conn, pq3.types.AuthnRequest, type=pq3.authn.SASLFinal)
            finish_handshake(conn)


@pytest.mark.parametrize(
    "response,expected_error",
    [
        pytest.param(
            "abcde",
            'Token "abcde" is invalid',
            id="bad JSON: invalid syntax",
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
            '{ "status": "invalid_token", "scope": 1 }',
            'field "scope" must be a string',
            id="bad JSON: int scope member",
        ),
    ],
)
def test_oauth_discovery_server_error(accept, response, expected_error):
    sock, client = accept(oauth_client_id=secrets.token_hex())

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            initial = start_oauth_handshake(conn)

            # Fail the SASL exchange with an invalid JSON response.
            pq3.send(
                conn,
                pq3.types.AuthnRequest,
                type=pq3.authn.SASLContinue,
                body=response.encode("utf-8"),
            )

            # The client should disconnect, so the socket is closed here. (If
            # the client doesn't disconnect, it will report a different error
            # below and the test will fail.)

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
            "expected error message",
            id="standard server error: invalid_request",
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
            {"status": "invalid_request"},
            pq3.types.AuthnRequest,
            dict(type=pq3.authn.SASLContinue, body=b""),
            "server sent additional OAuth data",
            id="broken server: additional challenge after error",
        ),
        pytest.param(
            {"status": "invalid_request"},
            pq3.types.AuthnRequest,
            dict(type=pq3.authn.SASLFinal),
            "server sent additional OAuth data",
            id="broken server: SASL success after error",
        ),
    ],
)
def test_oauth_server_error(accept, sasl_err, resp_type, resp_payload, expected_error):
    sock, client = accept()

    with sock:
        with pq3.wrap(sock, debug_stream=sys.stdout) as conn:
            start_oauth_handshake(conn)

            # Ignore the client data. Return an error "challenge".
            resp = json.dumps(sasl_err)
            resp = resp.encode("utf-8")

            pq3.send(
                conn, pq3.types.AuthnRequest, type=pq3.authn.SASLContinue, body=resp
            )

            # Per RFC, the client is required to send a dummy ^A response.
            pkt = pq3.recv1(conn)
            assert pkt.type == pq3.types.PasswordMessage
            assert pkt.payload == b"\x01"

            # Now fail the SASL exchange (in either a valid way, or an invalid
            # one, depending on the test).
            pq3.send(conn, resp_type, **resp_payload)

    with pytest.raises(psycopg2.OperationalError, match=expected_error):
        client.check_completed()
