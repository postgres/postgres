#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#

import base64
import contextlib
import json
import os
import pathlib
import secrets
import shlex
import shutil
import socket
import struct
from multiprocessing import shared_memory

import psycopg2
import pytest
from psycopg2 import sql

import pq3

MAX_SASL_MESSAGE_LENGTH = 65535

INVALID_AUTHORIZATION_ERRCODE = b"28000"
PROTOCOL_VIOLATION_ERRCODE = b"08P01"
FEATURE_NOT_SUPPORTED_ERRCODE = b"0A000"

SHARED_MEM_NAME = "oauth-pytest"
MAX_TOKEN_SIZE = 4096
MAX_UINT16 = 2 ** 16 - 1


def skip_if_no_postgres():
    """
    Used by the oauth_ctx fixture to skip this test module if no Postgres server
    is running.

    This logic is nearly duplicated with the conn fixture. Ideally oauth_ctx
    would depend on that, but a module-scope fixture can't depend on a
    test-scope fixture, and we haven't reached the rule of three yet.
    """
    addr = (pq3.pghost(), pq3.pgport())

    try:
        with socket.create_connection(addr, timeout=2):
            pass
    except ConnectionError as e:
        pytest.skip(f"unable to connect to {addr}: {e}")


@contextlib.contextmanager
def prepend_file(path, lines):
    """
    A context manager that prepends a file on disk with the desired lines of
    text. When the context manager is exited, the file will be restored to its
    original contents.
    """
    # First make a backup of the original file.
    bak = path + ".bak"
    shutil.copy2(path, bak)

    try:
        # Write the new lines, followed by the original file content.
        with open(path, "w") as new, open(bak, "r") as orig:
            new.writelines(lines)
            shutil.copyfileobj(orig, new)

        # Return control to the calling code.
        yield

    finally:
        # Put the backup back into place.
        os.replace(bak, path)


@pytest.fixture(scope="module")
def oauth_ctx():
    """
    Creates a database and user that use the oauth auth method. The context
    object contains the dbname and user attributes as strings to be used during
    connection, as well as the issuer and scope that have been set in the HBA
    configuration.

    This fixture assumes that the standard PG* environment variables point to a
    server running on a local machine, and that the PGUSER has rights to create
    databases and roles.
    """
    skip_if_no_postgres()  # don't bother running these tests without a server

    id = secrets.token_hex(4)

    class Context:
        dbname = "oauth_test_" + id

        user = "oauth_user_" + id
        map_user = "oauth_map_user_" + id
        authz_user = "oauth_authz_user_" + id

        issuer = "https://example.com/" + id
        scope = "openid " + id

    ctx = Context()
    hba_lines = (
        f'host {ctx.dbname} {ctx.map_user}   samehost oauth issuer="{ctx.issuer}" scope="{ctx.scope}" map=oauth\n',
        f'host {ctx.dbname} {ctx.authz_user} samehost oauth issuer="{ctx.issuer}" scope="{ctx.scope}" trust_validator_authz=1\n',
        f'host {ctx.dbname} all              samehost oauth issuer="{ctx.issuer}" scope="{ctx.scope}"\n',
    )
    ident_lines = (r"oauth /^(.*)@example\.com$ \1",)

    conn = psycopg2.connect("")
    conn.autocommit = True

    with contextlib.closing(conn):
        c = conn.cursor()

        # Create our roles and database.
        user = sql.Identifier(ctx.user)
        map_user = sql.Identifier(ctx.map_user)
        authz_user = sql.Identifier(ctx.authz_user)
        dbname = sql.Identifier(ctx.dbname)

        c.execute(sql.SQL("CREATE ROLE {} LOGIN;").format(user))
        c.execute(sql.SQL("CREATE ROLE {} LOGIN;").format(map_user))
        c.execute(sql.SQL("CREATE ROLE {} LOGIN;").format(authz_user))
        c.execute(sql.SQL("CREATE DATABASE {};").format(dbname))

        # Make this test script the server's oauth_validator.
        path = pathlib.Path(__file__).parent / "validate_bearer.py"
        path = str(path.absolute())

        cmd = f"{shlex.quote(path)} {SHARED_MEM_NAME} <&%f"
        c.execute("ALTER SYSTEM SET oauth_validator_command TO %s;", (cmd,))

        # Replace pg_hba and pg_ident.
        c.execute("SHOW hba_file;")
        hba = c.fetchone()[0]

        c.execute("SHOW ident_file;")
        ident = c.fetchone()[0]

        with prepend_file(hba, hba_lines), prepend_file(ident, ident_lines):
            c.execute("SELECT pg_reload_conf();")

            # Use the new database and user.
            yield ctx

        # Put things back the way they were.
        c.execute("SELECT pg_reload_conf();")

        c.execute("ALTER SYSTEM RESET oauth_validator_command;")
        c.execute(sql.SQL("DROP DATABASE {};").format(dbname))
        c.execute(sql.SQL("DROP ROLE {};").format(authz_user))
        c.execute(sql.SQL("DROP ROLE {};").format(map_user))
        c.execute(sql.SQL("DROP ROLE {};").format(user))


@pytest.fixture()
def conn(oauth_ctx, connect):
    """
    A convenience wrapper for connect(). The main purpose of this fixture is to
    make sure oauth_ctx runs its setup code before the connection is made.
    """
    return connect()


@pytest.fixture(scope="module", autouse=True)
def authn_id_extension(oauth_ctx):
    """
    Performs a `CREATE EXTENSION authn_id` in the test database. This fixture is
    autoused, so tests don't need to rely on it.
    """
    conn = psycopg2.connect(database=oauth_ctx.dbname)
    conn.autocommit = True

    with contextlib.closing(conn):
        c = conn.cursor()
        c.execute("CREATE EXTENSION authn_id;")


@pytest.fixture(scope="session")
def shared_mem():
    """
    Yields a shared memory segment that can be used for communication between
    the bearer_token fixture and ./validate_bearer.py.
    """
    size = MAX_TOKEN_SIZE + 2  # two byte length prefix
    mem = shared_memory.SharedMemory(SHARED_MEM_NAME, create=True, size=size)

    try:
        with contextlib.closing(mem):
            yield mem
    finally:
        mem.unlink()


@pytest.fixture()
def bearer_token(shared_mem):
    """
    Returns a factory function that, when called, will store a Bearer token in
    shared_mem. If token is None (the default), a new token will be generated
    using secrets.token_urlsafe() and returned; otherwise the passed token will
    be used as-is.

    When token is None, the generated token size in bytes may be specified as an
    argument; if unset, a small 16-byte token will be generated. The token size
    may not exceed MAX_TOKEN_SIZE in any case.

    The return value is the token, converted to a bytes object.

    As a special case for testing failure modes, accept_any may be set to True.
    This signals to the validator command that any bearer token should be
    accepted. The returned token in this case may be used or discarded as needed
    by the test.
    """

    def set_token(token=None, *, size=16, accept_any=False):
        if token is not None:
            size = len(token)

        if size > MAX_TOKEN_SIZE:
            raise ValueError(f"token size {size} exceeds maximum size {MAX_TOKEN_SIZE}")

        if token is None:
            if size % 4:
                raise ValueError(f"requested token size {size} is not a multiple of 4")

            token = secrets.token_urlsafe(size // 4 * 3)
            assert len(token) == size

        try:
            token = token.encode("ascii")
        except AttributeError:
            pass  # already encoded

        if accept_any:
            # Two-byte magic value.
            shared_mem.buf[:2] = struct.pack("H", MAX_UINT16)
        else:
            # Two-byte length prefix, then the token data.
            shared_mem.buf[:2] = struct.pack("H", len(token))
            shared_mem.buf[2 : size + 2] = token

        return token

    return set_token


def begin_oauth_handshake(conn, oauth_ctx, *, user=None):
    if user is None:
        user = oauth_ctx.authz_user

    pq3.send_startup(conn, user=user, database=oauth_ctx.dbname)

    resp = pq3.recv1(conn)
    assert resp.type == pq3.types.AuthnRequest

    # The server should advertise exactly one mechanism.
    assert resp.payload.type == pq3.authn.SASL
    assert resp.payload.body == [b"OAUTHBEARER", b""]


def send_initial_response(conn, *, auth=None, bearer=None):
    """
    Sends the OAUTHBEARER initial response on the connection, using the given
    bearer token. Alternatively to a bearer token, the initial response's auth
    field may be explicitly specified to test corner cases.
    """
    if bearer is not None and auth is not None:
        raise ValueError("exactly one of the auth and bearer kwargs must be set")

    if bearer is not None:
        auth = b"Bearer " + bearer

    if auth is None:
        raise ValueError("exactly one of the auth and bearer kwargs must be set")

    initial = pq3.SASLInitialResponse.build(
        dict(
            name=b"OAUTHBEARER",
            data=b"n,,\x01auth=" + auth + b"\x01\x01",
        )
    )
    pq3.send(conn, pq3.types.PasswordMessage, initial)


def expect_handshake_success(conn):
    """
    Validates that the server responds with an AuthnOK message, and then drains
    the connection until a ReadyForQuery message is received.
    """
    resp = pq3.recv1(conn)

    assert resp.type == pq3.types.AuthnRequest
    assert resp.payload.type == pq3.authn.OK
    assert not resp.payload.body

    receive_until(conn, pq3.types.ReadyForQuery)


def expect_handshake_failure(conn, oauth_ctx):
    """
    Performs the OAUTHBEARER SASL failure "handshake" and validates the server's
    side of the conversation, including the final ErrorResponse.
    """

    # We expect a discovery "challenge" back from the server before the authn
    # failure message.
    resp = pq3.recv1(conn)
    assert resp.type == pq3.types.AuthnRequest

    req = resp.payload
    assert req.type == pq3.authn.SASLContinue

    body = json.loads(req.body)
    assert body["status"] == "invalid_token"
    assert body["scope"] == oauth_ctx.scope

    expected_config = oauth_ctx.issuer + "/.well-known/openid-configuration"
    assert body["openid-configuration"] == expected_config

    # Send the dummy response to complete the failed handshake.
    pq3.send(conn, pq3.types.PasswordMessage, b"\x01")
    resp = pq3.recv1(conn)

    err = ExpectedError(INVALID_AUTHORIZATION_ERRCODE, "bearer authentication failed")
    err.match(resp)


def receive_until(conn, type):
    """
    receive_until pulls packets off the pq3 connection until a packet with the
    desired type is found, or an error response is received.
    """
    while True:
        pkt = pq3.recv1(conn)

        if pkt.type == type:
            return pkt
        elif pkt.type == pq3.types.ErrorResponse:
            raise RuntimeError(
                f"received error response from peer: {pkt.payload.fields!r}"
            )


@pytest.mark.parametrize("token_len", [16, 1024, 4096])
@pytest.mark.parametrize(
    "auth_prefix",
    [
        b"Bearer ",
        b"bearer ",
        b"Bearer    ",
    ],
)
def test_oauth(conn, oauth_ctx, bearer_token, auth_prefix, token_len):
    begin_oauth_handshake(conn, oauth_ctx)

    # Generate our bearer token with the desired length.
    token = bearer_token(size=token_len)
    auth = auth_prefix + token

    send_initial_response(conn, auth=auth)
    expect_handshake_success(conn)

    # Make sure that the server has not set an authenticated ID.
    pq3.send(conn, pq3.types.Query, query=b"SELECT authn_id();")
    resp = receive_until(conn, pq3.types.DataRow)

    row = resp.payload
    assert row.columns == [None]


@pytest.mark.parametrize(
    "token_value",
    [
        "abcdzA==",
        "123456M=",
        "x-._~+/x",
    ],
)
def test_oauth_bearer_corner_cases(conn, oauth_ctx, bearer_token, token_value):
    begin_oauth_handshake(conn, oauth_ctx)

    send_initial_response(conn, bearer=bearer_token(token_value))

    expect_handshake_success(conn)


@pytest.mark.parametrize(
    "user,authn_id,should_succeed",
    [
        pytest.param(
            lambda ctx: ctx.user,
            lambda ctx: ctx.user,
            True,
            id="validator authn: succeeds when authn_id == username",
        ),
        pytest.param(
            lambda ctx: ctx.user,
            lambda ctx: None,
            False,
            id="validator authn: fails when authn_id is not set",
        ),
        pytest.param(
            lambda ctx: ctx.user,
            lambda ctx: ctx.authz_user,
            False,
            id="validator authn: fails when authn_id != username",
        ),
        pytest.param(
            lambda ctx: ctx.map_user,
            lambda ctx: ctx.map_user + "@example.com",
            True,
            id="validator with map: succeeds when authn_id matches map",
        ),
        pytest.param(
            lambda ctx: ctx.map_user,
            lambda ctx: None,
            False,
            id="validator with map: fails when authn_id is not set",
        ),
        pytest.param(
            lambda ctx: ctx.map_user,
            lambda ctx: ctx.map_user + "@example.net",
            False,
            id="validator with map: fails when authn_id doesn't match map",
        ),
        pytest.param(
            lambda ctx: ctx.authz_user,
            lambda ctx: None,
            True,
            id="validator authz: succeeds with no authn_id",
        ),
        pytest.param(
            lambda ctx: ctx.authz_user,
            lambda ctx: "",
            True,
            id="validator authz: succeeds with empty authn_id",
        ),
        pytest.param(
            lambda ctx: ctx.authz_user,
            lambda ctx: "postgres",
            True,
            id="validator authz: succeeds with basic username",
        ),
        pytest.param(
            lambda ctx: ctx.authz_user,
            lambda ctx: "me@example.com",
            True,
            id="validator authz: succeeds with email address",
        ),
    ],
)
def test_oauth_authn_id(conn, oauth_ctx, bearer_token, user, authn_id, should_succeed):
    token = None

    authn_id = authn_id(oauth_ctx)
    if authn_id is not None:
        authn_id = authn_id.encode("ascii")

        # As a hack to get the validator to reflect arbitrary output from this
        # test, encode the desired output as a base64 token. The validator will
        # key on the leading "output=" to differentiate this from the random
        # tokens generated by secrets.token_urlsafe().
        output = b"output=" + authn_id + b"\n"
        token = base64.urlsafe_b64encode(output)

    token = bearer_token(token)
    username = user(oauth_ctx)

    begin_oauth_handshake(conn, oauth_ctx, user=username)
    send_initial_response(conn, bearer=token)

    if not should_succeed:
        expect_handshake_failure(conn, oauth_ctx)
        return

    expect_handshake_success(conn)

    # Check the reported authn_id.
    pq3.send(conn, pq3.types.Query, query=b"SELECT authn_id();")
    resp = receive_until(conn, pq3.types.DataRow)

    row = resp.payload
    assert row.columns == [authn_id]


class ExpectedError(object):
    def __init__(self, code, msg=None, detail=None):
        self.code = code
        self.msg = msg
        self.detail = detail

        # Protect against the footgun of an accidental empty string, which will
        # "match" anything. If you don't want to match message or detail, just
        # don't pass them.
        if self.msg == "":
            raise ValueError("msg must be non-empty or None")
        if self.detail == "":
            raise ValueError("detail must be non-empty or None")

    def _getfield(self, resp, type):
        """
        Searches an ErrorResponse for a single field of the given type (e.g.
        "M", "C", "D") and returns its value. Asserts if it doesn't find exactly
        one field.
        """
        prefix = type.encode("ascii")
        fields = [f for f in resp.payload.fields if f.startswith(prefix)]

        assert len(fields) == 1
        return fields[0][1:]  # strip off the type byte

    def match(self, resp):
        """
        Checks that the given response matches the expected code, message, and
        detail (if given). The error code must match exactly. The expected
        message and detail must be contained within the actual strings.
        """
        assert resp.type == pq3.types.ErrorResponse

        code = self._getfield(resp, "C")
        assert code == self.code

        if self.msg:
            msg = self._getfield(resp, "M")
            expected = self.msg.encode("utf-8")
            assert expected in msg

        if self.detail:
            detail = self._getfield(resp, "D")
            expected = self.detail.encode("utf-8")
            assert expected in detail


def test_oauth_rejected_bearer(conn, oauth_ctx, bearer_token):
    # Generate a new bearer token, which we will proceed not to use.
    _ = bearer_token()

    begin_oauth_handshake(conn, oauth_ctx)

    # Send a bearer token that doesn't match what the validator expects. It
    # should fail the connection.
    send_initial_response(conn, bearer=b"xxxxxx")

    expect_handshake_failure(conn, oauth_ctx)


@pytest.mark.parametrize(
    "bad_bearer",
    [
        b"Bearer    ",
        b"Bearer a===b",
        b"Bearer hello!",
        b"Bearer me@example.com",
        b'OAuth realm="Example"',
        b"",
    ],
)
def test_oauth_invalid_bearer(conn, oauth_ctx, bearer_token, bad_bearer):
    # Tell the validator to accept any token. This ensures that the invalid
    # bearer tokens are rejected before the validation step.
    _ = bearer_token(accept_any=True)

    begin_oauth_handshake(conn, oauth_ctx)
    send_initial_response(conn, auth=bad_bearer)

    expect_handshake_failure(conn, oauth_ctx)


@pytest.mark.slow
@pytest.mark.parametrize(
    "resp_type,resp,err",
    [
        pytest.param(
            None,
            None,
            None,
            marks=pytest.mark.slow,
            id="no response (expect timeout)",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            b"hello",
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "did not send a kvsep response",
            ),
            id="bad dummy response",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            b"\x01\x01",
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "did not send a kvsep response",
            ),
            id="multiple kvseps",
        ),
        pytest.param(
            pq3.types.Query,
            dict(query=b""),
            ExpectedError(PROTOCOL_VIOLATION_ERRCODE, "expected SASL response"),
            id="bad response message type",
        ),
    ],
)
def test_oauth_bad_response_to_error_challenge(conn, oauth_ctx, resp_type, resp, err):
    begin_oauth_handshake(conn, oauth_ctx)

    # Send an empty auth initial response, which will force an authn failure.
    send_initial_response(conn, auth=b"")

    # We expect a discovery "challenge" back from the server before the authn
    # failure message.
    pkt = pq3.recv1(conn)
    assert pkt.type == pq3.types.AuthnRequest

    req = pkt.payload
    assert req.type == pq3.authn.SASLContinue

    body = json.loads(req.body)
    assert body["status"] == "invalid_token"

    if resp_type is None:
        # Do not send the dummy response. We should time out and not get a
        # response from the server.
        with pytest.raises(socket.timeout):
            conn.read(1)

        # Done with the test.
        return

    # Send the bad response.
    pq3.send(conn, resp_type, resp)

    # Make sure the server fails the connection correctly.
    pkt = pq3.recv1(conn)
    err.match(pkt)


@pytest.mark.parametrize(
    "type,payload,err",
    [
        pytest.param(
            pq3.types.ErrorResponse,
            dict(fields=[b""]),
            ExpectedError(PROTOCOL_VIOLATION_ERRCODE, "expected SASL response"),
            id="error response in initial message",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            b"x" * (MAX_SASL_MESSAGE_LENGTH + 1),
            ExpectedError(
                INVALID_AUTHORIZATION_ERRCODE, "bearer authentication failed"
            ),
            id="overlong initial response data",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"SCRAM-SHA-256")),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE, "invalid SASL authentication mechanism"
            ),
            id="bad SASL mechanism selection",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", len=2, data=b"x")),
            ExpectedError(PROTOCOL_VIOLATION_ERRCODE, "insufficient data"),
            id="SASL data underflow",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", len=0, data=b"x")),
            ExpectedError(PROTOCOL_VIOLATION_ERRCODE, "invalid message format"),
            id="SASL data overflow",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", data=b"")),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "message is empty",
            ),
            id="empty",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(name=b"OAUTHBEARER", data=b"n,,\x01auth=\x01\x01\0")
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "length does not match input length",
            ),
            id="contains null byte",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", data=b"\x01")),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "Unexpected channel-binding flag",  # XXX this is a bit strange
            ),
            id="initial error response",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(name=b"OAUTHBEARER", data=b"p=tls-server-end-point,,\x01")
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "server does not support channel binding",
            ),
            id="uses channel binding",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", data=b"x,,\x01")),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "Unexpected channel-binding flag",
            ),
            id="invalid channel binding specifier",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", data=b"y")),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "Comma expected",
            ),
            id="bad GS2 header: missing channel binding terminator",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", data=b"y,a")),
            ExpectedError(
                FEATURE_NOT_SUPPORTED_ERRCODE,
                "client uses authorization identity",
            ),
            id="bad GS2 header: authzid in use",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", data=b"y,b,")),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "Unexpected attribute",
            ),
            id="bad GS2 header: extra attribute",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", data=b"y,")),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "Unexpected attribute 0x00",  # XXX this is a bit strange
            ),
            id="bad GS2 header: missing authzid terminator",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", data=b"y,,")),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "Key-value separator expected",
            ),
            id="missing initial kvsep",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", data=b"y,,")),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "Key-value separator expected",
            ),
            id="missing initial kvsep",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(name=b"OAUTHBEARER", data=b"y,,\x01\x01")
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "does not contain an auth value",
            ),
            id="missing auth value: empty key-value list",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(name=b"OAUTHBEARER", data=b"y,,\x01host=example.com\x01\x01")
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "does not contain an auth value",
            ),
            id="missing auth value: other keys present",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(name=b"OAUTHBEARER", data=b"y,,\x01host=example.com")
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "unterminated key/value pair",
            ),
            id="missing value terminator",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER", data=b"y,,\x01")),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "did not contain a final terminator",
            ),
            id="missing list terminator: empty list",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(name=b"OAUTHBEARER", data=b"y,,\x01auth=Bearer 0\x01")
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "did not contain a final terminator",
            ),
            id="missing list terminator: with auth value",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(name=b"OAUTHBEARER", data=b"y,,\x01auth=Bearer 0\x01\x01blah")
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "additional data after the final terminator",
            ),
            id="additional key after terminator",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(name=b"OAUTHBEARER", data=b"y,,\x01key\x01\x01")
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "key without a value",
            ),
            id="key without value",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(
                    name=b"OAUTHBEARER",
                    data=b"y,,\x01auth=Bearer 0\x01auth=Bearer 1\x01\x01",
                )
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "contains multiple auth values",
            ),
            id="multiple auth values",
        ),
    ],
)
def test_oauth_bad_initial_response(conn, oauth_ctx, type, payload, err):
    begin_oauth_handshake(conn, oauth_ctx)

    # The server expects a SASL response; give it something else instead.
    if not isinstance(payload, dict):
        payload = dict(payload_data=payload)
    pq3.send(conn, type, **payload)

    resp = pq3.recv1(conn)
    err.match(resp)


def test_oauth_empty_initial_response(conn, oauth_ctx, bearer_token):
    begin_oauth_handshake(conn, oauth_ctx)

    # Send an initial response without data.
    initial = pq3.SASLInitialResponse.build(dict(name=b"OAUTHBEARER"))
    pq3.send(conn, pq3.types.PasswordMessage, initial)

    # The server should respond with an empty challenge so we can send the data
    # it wants.
    pkt = pq3.recv1(conn)

    assert pkt.type == pq3.types.AuthnRequest
    assert pkt.payload.type == pq3.authn.SASLContinue
    assert not pkt.payload.body

    # Now send the initial data.
    data = b"n,,\x01auth=Bearer " + bearer_token() + b"\x01\x01"
    pq3.send(conn, pq3.types.PasswordMessage, data)

    # Server should now complete the handshake.
    expect_handshake_success(conn)


@pytest.fixture()
def set_validator():
    """
    A per-test fixture that allows a test to override the setting of
    oauth_validator_command for the cluster. The setting will be reverted during
    teardown.

    Passing None will perform an ALTER SYSTEM RESET.
    """
    conn = psycopg2.connect("")
    conn.autocommit = True

    with contextlib.closing(conn):
        c = conn.cursor()

        # Save the previous value.
        c.execute("SHOW oauth_validator_command;")
        prev_cmd = c.fetchone()[0]

        def setter(cmd):
            c.execute("ALTER SYSTEM SET oauth_validator_command TO %s;", (cmd,))
            c.execute("SELECT pg_reload_conf();")

        yield setter

        # Restore the previous value.
        c.execute("ALTER SYSTEM SET oauth_validator_command TO %s;", (prev_cmd,))
        c.execute("SELECT pg_reload_conf();")


def test_oauth_no_validator(oauth_ctx, set_validator, connect, bearer_token):
    # Clear out our validator command, then establish a new connection.
    set_validator("")
    conn = connect()

    begin_oauth_handshake(conn, oauth_ctx)
    send_initial_response(conn, bearer=bearer_token())

    # The server should fail the connection.
    expect_handshake_failure(conn, oauth_ctx)


def test_oauth_validator_role(oauth_ctx, set_validator, connect):
    # Switch the validator implementation. This validator will reflect the
    # PGUSER as the authenticated identity.
    path = pathlib.Path(__file__).parent / "validate_reflect.py"
    path = str(path.absolute())

    set_validator(f"{shlex.quote(path)} '%r' <&%f")
    conn = connect()

    # Log in. Note that the reflection validator ignores the bearer token.
    begin_oauth_handshake(conn, oauth_ctx, user=oauth_ctx.user)
    send_initial_response(conn, bearer=b"dontcare")
    expect_handshake_success(conn)

    # Check the user identity.
    pq3.send(conn, pq3.types.Query, query=b"SELECT authn_id();")
    resp = receive_until(conn, pq3.types.DataRow)

    row = resp.payload
    expected = oauth_ctx.user.encode("utf-8")
    assert row.columns == [expected]


def test_oauth_role_with_shell_unsafe_characters(oauth_ctx, set_validator, connect):
    """
    XXX This test pins undesirable behavior. We should be able to handle any
    valid Postgres role name.
    """
    # Switch the validator implementation. This validator will reflect the
    # PGUSER as the authenticated identity.
    path = pathlib.Path(__file__).parent / "validate_reflect.py"
    path = str(path.absolute())

    set_validator(f"{shlex.quote(path)} '%r' <&%f")
    conn = connect()

    unsafe_username = "hello'there"
    begin_oauth_handshake(conn, oauth_ctx, user=unsafe_username)

    # The server should reject the handshake.
    send_initial_response(conn, bearer=b"dontcare")
    expect_handshake_failure(conn, oauth_ctx)
