#! /usr/bin/env python3
#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#
# DO NOT USE THIS OAUTH VALIDATOR IN PRODUCTION. It ignores the bearer token
# entirely and automatically logs the user in.
#
# This executable is used as an oauth_validator_command in concert with
# test_oauth.py. It expects the user's desired role name as an argument; the
# actual token will be discarded and the user will be logged in with the role
# name as the authenticated identity.
#
# This script must run under the Postgres server environment; keep the
# dependency list fairly standard.

import sys


def main(args):
    # We have to read the entire token as our first action to unblock the
    # server, but we won't actually use it.
    _ = sys.stdin.buffer.read()

    if len(args) != 1:
        sys.exit("usage: ./validate_reflect.py ROLE")

    # Log the user in as the provided role.
    role = args[0]
    print(role)


if __name__ == "__main__":
    main(sys.argv[1:])
