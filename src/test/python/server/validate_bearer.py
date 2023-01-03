#! /usr/bin/env python3
#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#
# DO NOT USE THIS OAUTH VALIDATOR IN PRODUCTION. It doesn't actually validate
# anything, and it logs the bearer token data, which is sensitive.
#
# This executable is used as an oauth_validator_command in concert with
# test_oauth.py. Memory is shared and communicated from that test module's
# bearer_token() fixture.
#
# This script must run under the Postgres server environment; keep the
# dependency list fairly standard.

import base64
import binascii
import contextlib
import struct
import sys
from multiprocessing import shared_memory

MAX_UINT16 = 2 ** 16 - 1


def remove_shm_from_resource_tracker():
    """
    Monkey-patch multiprocessing.resource_tracker so SharedMemory won't be
    tracked. Pulled from this thread, where there are more details:

        https://bugs.python.org/issue38119

    TL;DR: all clients of shared memory segments automatically destroy them on
    process exit, which makes shared memory segments much less useful. This
    monkeypatch removes that behavior so that we can defer to the test to manage
    the segment lifetime.

    Ideally a future Python patch will pull in this fix and then the entire
    function can go away.
    """
    from multiprocessing import resource_tracker

    def fix_register(name, rtype):
        if rtype == "shared_memory":
            return
        return resource_tracker._resource_tracker.register(self, name, rtype)

    resource_tracker.register = fix_register

    def fix_unregister(name, rtype):
        if rtype == "shared_memory":
            return
        return resource_tracker._resource_tracker.unregister(self, name, rtype)

    resource_tracker.unregister = fix_unregister

    if "shared_memory" in resource_tracker._CLEANUP_FUNCS:
        del resource_tracker._CLEANUP_FUNCS["shared_memory"]


def main(args):
    remove_shm_from_resource_tracker()  # XXX remove some day

    # Get the expected token from the currently running test.
    shared_mem_name = args[0]

    mem = shared_memory.SharedMemory(shared_mem_name)
    with contextlib.closing(mem):
        # First two bytes are the token length.
        size = struct.unpack("H", mem.buf[:2])[0]

        if size == MAX_UINT16:
            # Special case: the test wants us to accept any token.
            sys.stderr.write("accepting token without validation\n")
            return

        # The remainder of the buffer contains the expected token.
        assert size <= (mem.size - 2)
        expected_token = mem.buf[2 : size + 2].tobytes()

        mem.buf[:] = b"\0" * mem.size  # scribble over the token

    token = sys.stdin.buffer.read()
    if token != expected_token:
        sys.exit(f"failed to match Bearer token ({token!r} != {expected_token!r})")

    # See if the test wants us to print anything. If so, it will have encoded
    # the desired output in the token with an "output=" prefix.
    try:
        # altchars="-_" corresponds to the urlsafe alphabet.
        data = base64.b64decode(token, altchars="-_", validate=True)

        if data.startswith(b"output="):
            sys.stdout.buffer.write(data[7:])

    except binascii.Error:
        pass


if __name__ == "__main__":
    main(sys.argv[1:])
