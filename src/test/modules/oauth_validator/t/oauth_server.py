#! /usr/bin/env python3
#
# A mock OAuth authorization server, designed to be invoked from
# OAuth/Server.pm. This listens on an ephemeral port number (printed to stdout
# so that the Perl tests can contact it) and runs as a daemon until it is
# signaled.
#

import base64
import functools
import http.server
import json
import os
import sys
import time
import urllib.parse
from collections import defaultdict
from typing import Dict


class OAuthHandler(http.server.BaseHTTPRequestHandler):
    """
    Core implementation of the authorization server. The API is
    inheritance-based, with entry points at do_GET() and do_POST(). See the
    documentation for BaseHTTPRequestHandler.
    """

    JsonObject = Dict[str, object]  # TypeAlias is not available until 3.10

    def _check_issuer(self):
        """
        Switches the behavior of the provider depending on the issuer URI.
        """
        self._alt_issuer = (
            self.path.startswith("/alternate/")
            or self.path == "/.well-known/oauth-authorization-server/alternate"
        )
        self._parameterized = self.path.startswith("/param/")

        # Strip off the magic path segment. (The more readable
        # str.removeprefix()/removesuffix() aren't available until Py3.9.)
        if self._alt_issuer:
            # The /alternate issuer uses IETF-style .well-known URIs.
            if self.path.startswith("/.well-known/"):
                self.path = self.path[: -len("/alternate")]
            else:
                self.path = self.path[len("/alternate") :]
        elif self._parameterized:
            self.path = self.path[len("/param") :]

    def _check_authn(self):
        """
        Checks the expected value of the Authorization header, if any.
        """
        secret = self._get_param("expected_secret", None)
        if secret is None:
            return

        assert "Authorization" in self.headers
        method, creds = self.headers["Authorization"].split()

        if method != "Basic":
            raise RuntimeError(f"client used {method} auth; expected Basic")

        # TODO: Remove "~" from the safe list after Py3.6 support is removed.
        # 3.7 does this by default.
        username = urllib.parse.quote_plus(self.client_id, safe="~")
        password = urllib.parse.quote_plus(secret, safe="~")
        expected_creds = f"{username}:{password}"

        if creds.encode() != base64.b64encode(expected_creds.encode()):
            raise RuntimeError(
                f"client sent '{creds}'; expected b64encode('{expected_creds}')"
            )

    def do_GET(self):
        self._response_code = 200
        self._check_issuer()

        config_path = "/.well-known/openid-configuration"
        if self._alt_issuer:
            config_path = "/.well-known/oauth-authorization-server"

        if self.path == config_path:
            resp = self.config()
        else:
            self.send_error(404, "Not Found")
            return

        self._send_json(resp)

    def _parse_params(self) -> Dict[str, str]:
        """
        Parses apart the form-urlencoded request body and returns the resulting
        dict. For use by do_POST().
        """
        size = int(self.headers["Content-Length"])
        form = self.rfile.read(size)

        assert self.headers["Content-Type"] == "application/x-www-form-urlencoded"
        return urllib.parse.parse_qs(
            form.decode("utf-8"),
            strict_parsing=True,
            keep_blank_values=True,
            encoding="utf-8",
            errors="strict",
        )

    @property
    def client_id(self) -> str:
        """
        Returns the client_id sent in the POST body or the Authorization header.
        self._parse_params() must have been called first.
        """
        if "client_id" in self._params:
            return self._params["client_id"][0]

        if "Authorization" not in self.headers:
            raise RuntimeError("client did not send any client_id")

        _, creds = self.headers["Authorization"].split()

        decoded = base64.b64decode(creds).decode("utf-8")
        username, _ = decoded.split(":", 1)

        return urllib.parse.unquote_plus(username)

    def do_POST(self):
        self._response_code = 200
        self._check_issuer()

        self._params = self._parse_params()
        if self._parameterized:
            # Pull encoded test parameters out of the peer's client_id field.
            # This is expected to be Base64-encoded JSON.
            js = base64.b64decode(self.client_id)
            self._test_params = json.loads(js)

        self._check_authn()

        if self.path == "/authorize":
            resp = self.authorization()
        elif self.path == "/token":
            resp = self.token()
        else:
            self.send_error(404)
            return

        self._send_json(resp)

    def _should_modify(self) -> bool:
        """
        Returns True if the client has requested a modification to this stage of
        the exchange.
        """
        if not hasattr(self, "_test_params"):
            return False

        stage = self._test_params.get("stage")

        return (
            stage == "all"
            or (
                stage == "discovery"
                and self.path == "/.well-known/openid-configuration"
            )
            or (stage == "device" and self.path == "/authorize")
            or (stage == "token" and self.path == "/token")
        )

    def _get_param(self, name, default):
        """
        If the client has requested a modification to this stage (see
        _should_modify()), this method searches the provided test parameters for
        a key of the given name, and returns it if found. Otherwise the provided
        default is returned.
        """
        if self._should_modify() and name in self._test_params:
            return self._test_params[name]

        return default

    @property
    def _content_type(self) -> str:
        """
        Returns "application/json" unless the test has requested something
        different.
        """
        return self._get_param("content_type", "application/json")

    @property
    def _interval(self) -> int:
        """
        Returns 0 unless the test has requested something different.
        """
        return self._get_param("interval", 0)

    @property
    def _retry_code(self) -> str:
        """
        Returns "authorization_pending" unless the test has requested something
        different.
        """
        return self._get_param("retry_code", "authorization_pending")

    @property
    def _uri_spelling(self) -> str:
        """
        Returns "verification_uri" unless the test has requested something
        different.
        """
        return self._get_param("uri_spelling", "verification_uri")

    @property
    def _response_padding(self):
        """
        Returns a dict with any additional entries that should be folded into a
        JSON response, as determined by test parameters provided by the client:

        - huge_response: if set to True, the dict will contain a gigantic string
          value

        - nested_array: if set to nonzero, the dict will contain a deeply nested
          array so that the top-level object has the given depth

        - nested_object: if set to nonzero, the dict will contain a deeply
          nested JSON object so that the top-level object has the given depth
        """
        ret = dict()

        if self._get_param("huge_response", False):
            ret["_pad_"] = "x" * 1024 * 1024

        depth = self._get_param("nested_array", 0)
        if depth:
            ret["_arr_"] = functools.reduce(lambda x, _: [x], range(depth))

        depth = self._get_param("nested_object", 0)
        if depth:
            ret["_obj_"] = functools.reduce(lambda x, _: {"": x}, range(depth))

        return ret

    @property
    def _access_token(self):
        """
        The actual Bearer token sent back to the client on success. Tests may
        override this with the "token" test parameter.
        """
        token = self._get_param("token", None)
        if token is not None:
            return token

        token = "9243959234"
        if self._alt_issuer:
            token += "-alt"

        return token

    def _send_json(self, js: JsonObject) -> None:
        """
        Sends the provided JSON dict as an application/json response.
        self._response_code can be modified to send JSON error responses.
        """
        resp = json.dumps(js).encode("ascii")
        self.log_message("sending JSON response: %s", resp)

        self.send_response(self._response_code)
        self.send_header("Content-Type", self._content_type)
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()

        self.wfile.write(resp)

    def config(self) -> JsonObject:
        port = self.server.socket.getsockname()[1]

        issuer = f"http://127.0.0.1:{port}"
        if self._alt_issuer:
            issuer += "/alternate"
        elif self._parameterized:
            issuer += "/param"

        return {
            "issuer": issuer,
            "token_endpoint": issuer + "/token",
            "device_authorization_endpoint": issuer + "/authorize",
            "response_types_supported": ["token"],
            "subject_types_supported": ["public"],
            "id_token_signing_alg_values_supported": ["RS256"],
            "grant_types_supported": [
                "authorization_code",
                "urn:ietf:params:oauth:grant-type:device_code",
            ],
        }

    @property
    def _token_state(self):
        """
        A cached _TokenState object for the connected client (as determined by
        the request's client_id), or a new one if it doesn't already exist.

        This relies on the existence of a defaultdict attached to the server;
        see main() below.
        """
        return self.server.token_state[self.client_id]

    def _remove_token_state(self):
        """
        Removes any cached _TokenState for the current client_id. Call this
        after the token exchange ends to get rid of unnecessary state.
        """
        if self.client_id in self.server.token_state:
            del self.server.token_state[self.client_id]

    def authorization(self) -> JsonObject:
        uri = "https://example.com/"
        if self._alt_issuer:
            uri = "https://example.org/"

        resp = {
            "device_code": "postgres",
            "user_code": "postgresuser",
            self._uri_spelling: uri,
            "expires_in": 5,
            **self._response_padding,
        }

        interval = self._interval
        if interval is not None:
            resp["interval"] = interval
            self._token_state.min_delay = interval
        else:
            self._token_state.min_delay = 5  # default

        # Check the scope.
        if "scope" in self._params:
            assert self._params["scope"][0], "empty scopes should be omitted"

        return resp

    def token(self) -> JsonObject:
        err = self._get_param("error_code", None)
        if err:
            self._response_code = self._get_param("error_status", 400)

            resp = {"error": err}

            desc = self._get_param("error_desc", "")
            if desc:
                resp["error_description"] = desc

            return resp

        if self._should_modify() and "retries" in self._test_params:
            retries = self._test_params["retries"]

            # Check to make sure the token interval is being respected.
            now = time.monotonic()
            if self._token_state.last_try is not None:
                delay = now - self._token_state.last_try
                assert (
                    delay > self._token_state.min_delay
                ), f"client waited only {delay} seconds between token requests (expected {self._token_state.min_delay})"

            self._token_state.last_try = now

            # If we haven't reached the required number of retries yet, return a
            # "pending" response.
            if self._token_state.retries < retries:
                self._token_state.retries += 1

                self._response_code = 400
                return {"error": self._retry_code}

        # Clean up any retry tracking state now that the exchange is ending.
        self._remove_token_state()

        return {
            "access_token": self._access_token,
            "token_type": "bearer",
            **self._response_padding,
        }


def main():
    """
    Starts the authorization server on localhost. The ephemeral port in use will
    be printed to stdout.
    """

    s = http.server.HTTPServer(("127.0.0.1", 0), OAuthHandler)

    # Attach a "cache" dictionary to the server to allow the OAuthHandlers to
    # track state across token requests. The use of defaultdict ensures that new
    # entries will be created automatically.
    class _TokenState:
        retries = 0
        min_delay = None
        last_try = None

    s.token_state = defaultdict(_TokenState)

    # Give the parent the port number to contact (this is also the signal that
    # we're ready to receive requests).
    port = s.socket.getsockname()[1]
    print(port)

    # stdout is closed to allow the parent to just "read to the end".
    stdout = sys.stdout.fileno()
    sys.stdout.close()
    os.close(stdout)

    s.serve_forever()  # we expect our parent to send a termination signal


if __name__ == "__main__":
    main()
