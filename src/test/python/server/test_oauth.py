#
# Copyright 2021 VMware, Inc.
# Portions Copyright 2023 Timescale, Inc.
# SPDX-License-Identifier: PostgreSQL
#

import base64
import contextlib
import json
import os
import pathlib
import platform
import secrets
import shlex
import shutil
import socket
import struct
from multiprocessing import shared_memory

import psycopg2
import pytest
from construct import Container
from psycopg2 import sql

import pq3

from .conftest import BLOCKING_TIMEOUT

MAX_SASL_MESSAGE_LENGTH = 65535

INVALID_AUTHORIZATION_ERRCODE = b"28000"
PROTOCOL_VIOLATION_ERRCODE = b"08P01"
FEATURE_NOT_SUPPORTED_ERRCODE = b"0A000"

SHARED_MEM_NAME = "oauth-pytest"
MAX_UINT16 = 2**16 - 1


@contextlib.contextmanager
def prepend_file(path, lines, *, suffix=".bak"):
    """
    A context manager that prepends a file on disk with the desired lines of
    text. When the context manager is exited, the file will be restored to its
    original contents.
    """
    # First make a backup of the original file.
    bak = path + suffix
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
def oauth_ctx(postgres_instance):
    """
    Creates a database and user that use the oauth auth method. The context
    object contains the dbname and user attributes as strings to be used during
    connection, as well as the issuer and scope that have been set in the HBA
    configuration.

    This fixture assumes that the standard PG* environment variables point to a
    server running on a local machine, and that the PGUSER has rights to create
    databases and roles.
    """
    id = secrets.token_hex(4)

    class Context:
        dbname = "oauth_test_" + id

        user = "oauth_user_" + id
        punct_user = "oauth_\"'? ;&!_user_" + id  # username w/ punctuation
        map_user = "oauth_map_user_" + id
        authz_user = "oauth_authz_user_" + id

        issuer = "https://example.com/" + id
        scope = "openid " + id

    ctx = Context()
    hba_lines = [
        f'host {ctx.dbname} {ctx.map_user}   samehost oauth issuer="{ctx.issuer}" scope="{ctx.scope}" map=oauth\n',
        f'host {ctx.dbname} {ctx.authz_user} samehost oauth issuer="{ctx.issuer}" scope="{ctx.scope}" delegate_ident_mapping=1\n',
        f'host {ctx.dbname} all              samehost oauth issuer="{ctx.issuer}" scope="{ctx.scope}"\n',
    ]
    ident_lines = [r"oauth /^(.*)@example\.com$ \1"]

    if platform.system() == "Windows":
        # XXX why is 'samehost' not behaving as expected on Windows?
        for l in list(hba_lines):
            hba_lines.append(l.replace("samehost", "::1/128"))

    host, port = postgres_instance
    conn = psycopg2.connect(host=host, port=port)
    conn.autocommit = True

    with contextlib.closing(conn):
        c = conn.cursor()

        # Create our roles and database.
        user = sql.Identifier(ctx.user)
        punct_user = sql.Identifier(ctx.punct_user)
        map_user = sql.Identifier(ctx.map_user)
        authz_user = sql.Identifier(ctx.authz_user)
        dbname = sql.Identifier(ctx.dbname)

        c.execute(sql.SQL("CREATE ROLE {} LOGIN;").format(user))
        c.execute(sql.SQL("CREATE ROLE {} LOGIN;").format(punct_user))
        c.execute(sql.SQL("CREATE ROLE {} LOGIN;").format(map_user))
        c.execute(sql.SQL("CREATE ROLE {} LOGIN;").format(authz_user))
        c.execute(sql.SQL("CREATE DATABASE {};").format(dbname))

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

        c.execute(sql.SQL("DROP DATABASE {};").format(dbname))
        c.execute(sql.SQL("DROP ROLE {};").format(authz_user))
        c.execute(sql.SQL("DROP ROLE {};").format(map_user))
        c.execute(sql.SQL("DROP ROLE {};").format(punct_user))
        c.execute(sql.SQL("DROP ROLE {};").format(user))


@pytest.fixture()
def conn(oauth_ctx, connect):
    """
    A convenience wrapper for connect(). The main purpose of this fixture is to
    make sure oauth_ctx runs its setup code before the connection is made.
    """
    return connect()


def bearer_token(*, size=16):
    """
    Generates a Bearer token using secrets.token_urlsafe(). The generated token
    size in bytes may be specified; if unset, a small 16-byte token will be
    generated.
    """

    if size % 4:
        raise ValueError(f"requested token size {size} is not a multiple of 4")

    token = secrets.token_urlsafe(size // 4 * 3)
    assert len(token) == size

    return token


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


@pytest.fixture()
def setup_validator(postgres_instance):
    """
    A per-test fixture that sets up the test validator with expected behavior.
    The setting will be reverted during teardown.
    """
    host, port = postgres_instance
    conn = psycopg2.connect(host=host, port=port)
    conn.autocommit = True

    with contextlib.closing(conn):
        c = conn.cursor()
        prev = dict()

        def setter(**gucs):
            for guc, val in gucs.items():
                # Save the previous value.
                c.execute(sql.SQL("SHOW oauthtest.{};").format(sql.Identifier(guc)))
                prev[guc] = c.fetchone()[0]

                c.execute(
                    sql.SQL("ALTER SYSTEM SET oauthtest.{} TO %s;").format(
                        sql.Identifier(guc)
                    ),
                    (val,),
                )
                c.execute("SELECT pg_reload_conf();")

        yield setter

        # Restore the previous values.
        for guc, val in prev.items():
            c.execute(
                sql.SQL("ALTER SYSTEM SET oauthtest.{} TO %s;").format(
                    sql.Identifier(guc)
                ),
                (val,),
            )
            c.execute("SELECT pg_reload_conf();")


@pytest.mark.parametrize("token_len", [16, 1024, 4096])
@pytest.mark.parametrize(
    "auth_prefix",
    [
        b"Bearer ",
        b"bearer ",
        b"Bearer    ",
    ],
)
def test_oauth(setup_validator, connect, oauth_ctx, auth_prefix, token_len):
    # Generate our bearer token with the desired length.
    token = bearer_token(size=token_len)
    setup_validator(expected_bearer=token)

    conn = connect()
    begin_oauth_handshake(conn, oauth_ctx)

    auth = auth_prefix + token.encode("ascii")
    send_initial_response(conn, auth=auth)
    expect_handshake_success(conn)

    # Make sure that the server has not set an authenticated ID.
    pq3.send(conn, pq3.types.Query, query=b"SELECT system_user;")
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
def test_oauth_bearer_corner_cases(setup_validator, connect, oauth_ctx, token_value):
    setup_validator(expected_bearer=token_value)

    conn = connect()
    begin_oauth_handshake(conn, oauth_ctx)

    send_initial_response(conn, bearer=token_value.encode("ascii"))

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
            lambda ctx: "",
            False,
            id="validator authn: fails when authn_id is empty",
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
def test_oauth_authn_id(
    setup_validator, connect, oauth_ctx, user, authn_id, should_succeed
):
    token = bearer_token()
    authn_id = authn_id(oauth_ctx)

    # Set up the validator appropriately.
    gucs = dict(expected_bearer=token)
    if authn_id is not None:
        gucs["set_authn_id"] = True
        gucs["authn_id"] = authn_id
    setup_validator(**gucs)

    conn = connect()
    username = user(oauth_ctx)
    begin_oauth_handshake(conn, oauth_ctx, user=username)
    send_initial_response(conn, bearer=token.encode("ascii"))

    if not should_succeed:
        expect_handshake_failure(conn, oauth_ctx)
        return

    expect_handshake_success(conn)

    # Check the reported authn_id.
    pq3.send(conn, pq3.types.Query, query=b"SELECT system_user;")
    resp = receive_until(conn, pq3.types.DataRow)

    expected = authn_id
    if expected is not None:
        expected = b"oauth:" + expected.encode("ascii")

    row = resp.payload
    assert row.columns == [expected]


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

        assert len(fields) == 1, f"did not find exactly one {type} field"
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


def test_oauth_rejected_bearer(conn, oauth_ctx):
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
        b"Bearer trailingspace ",
        b"Bearer trailingtab\t",
        b"Bearer me@example.com",
        b"Beare abcd",
        b" Bearer leadingspace",
        b'OAuth realm="Example"',
        b"",
    ],
)
def test_oauth_invalid_bearer(setup_validator, connect, oauth_ctx, bad_bearer):
    # Tell the validator to accept any token. This ensures that the invalid
    # bearer tokens are rejected before the validation step.
    setup_validator(reflect_role=True)

    conn = connect()
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
            None,
            # Sending an actual 65k packet results in ECONNRESET on Windows, and
            # it floods the tests' connection log uselessly, so just fake the
            # length and send a smaller number of bytes.
            dict(
                type=pq3.types.PasswordMessage,
                len=MAX_SASL_MESSAGE_LENGTH + 1,
                payload=b"x" * 512,
            ),
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
                'Unexpected attribute "0x00"',  # XXX this is a bit strange
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
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(
                    name=b"OAUTHBEARER",
                    data=b"y,,\x01=\x01\x01",
                )
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "empty key name",
            ),
            id="empty key",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(
                    name=b"OAUTHBEARER",
                    data=b"y,,\x01my key= \x01\x01",
                )
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "invalid key name",
            ),
            id="whitespace in key name",
        ),
        pytest.param(
            pq3.types.PasswordMessage,
            pq3.SASLInitialResponse.build(
                dict(
                    name=b"OAUTHBEARER",
                    data=b"y,,\x01key=a\x05b\x01\x01",
                )
            ),
            ExpectedError(
                PROTOCOL_VIOLATION_ERRCODE,
                "malformed OAUTHBEARER message",
                "invalid value",
            ),
            id="junk in value",
        ),
    ],
)
def test_oauth_bad_initial_response(conn, oauth_ctx, type, payload, err):
    begin_oauth_handshake(conn, oauth_ctx)

    # The server expects a SASL response; give it something else instead.
    if type is not None:
        # Build a new packet of the desired type.
        if not isinstance(payload, dict):
            payload = dict(payload_data=payload)
        pq3.send(conn, type, **payload)
    else:
        # The test has a custom packet to send. (The only reason to do this is
        # if the packet is corrupt or otherwise unbuildable/unparsable, so we
        # don't use the standard pq3.send().)
        conn.write(pq3.Pq3.build(payload))
        conn.end_packet(Container(payload))

    resp = pq3.recv1(conn)
    err.match(resp)


def test_oauth_empty_initial_response(setup_validator, connect, oauth_ctx):
    token = bearer_token()
    setup_validator(expected_bearer=token)

    conn = connect()
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
    data = b"n,,\x01auth=Bearer " + token.encode("ascii") + b"\x01\x01"
    pq3.send(conn, pq3.types.PasswordMessage, data)

    # Server should now complete the handshake.
    expect_handshake_success(conn)


# TODO: see if there's a way to test this easily after the API switch
def xtest_oauth_no_validator(setup_validator, oauth_ctx, connect):
    # Clear out our validator command, then establish a new connection.
    set_validator("")
    conn = connect()

    begin_oauth_handshake(conn, oauth_ctx)
    send_initial_response(conn, bearer=bearer_token())

    # The server should fail the connection.
    expect_handshake_failure(conn, oauth_ctx)


@pytest.mark.parametrize(
    "user",
    [
        pytest.param(
            lambda ctx: ctx.user,
            id="basic username",
        ),
        pytest.param(
            lambda ctx: ctx.punct_user,
            id="'unsafe' characters are passed through correctly",
        ),
    ],
)
def test_oauth_validator_role(setup_validator, oauth_ctx, connect, user):
    username = user(oauth_ctx)

    # Tell the validator to reflect the PGUSER as the authenticated identity.
    setup_validator(reflect_role=True)
    conn = connect()

    # Log in. Note that reflection ignores the bearer token.
    begin_oauth_handshake(conn, oauth_ctx, user=username)
    send_initial_response(conn, bearer=b"dontcare")
    expect_handshake_success(conn)

    # Check the user identity.
    pq3.send(conn, pq3.types.Query, query=b"SELECT system_user;")
    resp = receive_until(conn, pq3.types.DataRow)

    row = resp.payload
    expected = b"oauth:" + username.encode("utf-8")
    assert row.columns == [expected]


@pytest.fixture
def odd_oauth_ctx(postgres_instance, oauth_ctx):
    """
    Adds an HBA entry with messed up issuer/scope settings, to pin the server
    behavior.

    TODO: these should really be rejected in the HBA rather than passed through
    by the server.
    """
    id = secrets.token_hex(4)

    class Context:
        user = oauth_ctx.user
        dbname = oauth_ctx.dbname

        # Both of these embedded double-quotes are invalid; they're prohibited
        # in both URLs and OAuth scope identifiers.
        issuer = oauth_ctx.issuer + '/"/'
        scope = oauth_ctx.scope + ' quo"ted'

    ctx = Context()
    hba_issuer = ctx.issuer.replace('"', '""')
    hba_scope = ctx.scope.replace('"', '""')
    hba_lines = [
        f'host {ctx.dbname} {ctx.user} samehost oauth issuer="{hba_issuer}" scope="{hba_scope}"\n',
    ]

    if platform.system() == "Windows":
        # XXX why is 'samehost' not behaving as expected on Windows?
        for l in list(hba_lines):
            hba_lines.append(l.replace("samehost", "::1/128"))

    host, port = postgres_instance
    conn = psycopg2.connect(host=host, port=port)
    conn.autocommit = True

    with contextlib.closing(conn):
        c = conn.cursor()

        # Replace pg_hba. Note that it's already been replaced once by
        # oauth_ctx, so use a different backup prefix in prepend_file().
        c.execute("SHOW hba_file;")
        hba = c.fetchone()[0]

        with prepend_file(hba, hba_lines, suffix=".bak2"):
            c.execute("SELECT pg_reload_conf();")

            yield ctx

        # Put things back the way they were.
        c.execute("SELECT pg_reload_conf();")


def test_odd_server_response(odd_oauth_ctx, connect):
    """
    Verifies that the server is correctly escaping the JSON in its failure
    response.
    """
    conn = connect()
    begin_oauth_handshake(conn, odd_oauth_ctx, user=odd_oauth_ctx.user)

    # Send an empty auth initial response, which will force an authn failure.
    send_initial_response(conn, auth=b"")

    expect_handshake_failure(conn, odd_oauth_ctx)
