#
# Copyright 2023 Timescale, Inc.
# SPDX-License-Identifier: PostgreSQL
#

import os

import pytest


def pytest_addoption(parser):
    """
    Adds custom command line options to py.test. We add one to signal temporary
    Postgres instance creation for the server tests.

    Per pytest documentation, this must live in the top level test directory.
    """
    parser.addoption(
        "--temp-instance",
        metavar="DIR",
        help="create a temporary Postgres instance in DIR",
    )


@pytest.fixture(scope="session", autouse=True)
def _check_PG_TEST_EXTRA(request):
    """
    Automatically skips the whole suite if PG_TEST_EXTRA doesn't contain
    'python'. pytestmark doesn't seem to work in a top-level conftest.py, so
    I've made this an autoused fixture instead.
    """
    extra_tests = os.getenv("PG_TEST_EXTRA", "").split()
    if "python" not in extra_tests:
        pytest.skip("Potentially unsafe test 'python' not enabled in PG_TEST_EXTRA")
