#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#

import io

from pq3 import _DebugStream


def test_DebugStream_read():
    under = io.BytesIO(b"abcdefghijklmnopqrstuvwxyz")
    out = io.StringIO()

    stream = _DebugStream(under, out)

    res = stream.read(5)
    assert res == b"abcde"

    res = stream.read(16)
    assert res == b"fghijklmnopqrstu"

    stream.flush_debug()

    res = stream.read()
    assert res == b"vwxyz"

    stream.flush_debug()

    expected = (
        "< 0000:\t61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f 70\tabcdefghijklmnop\n"
        "< 0010:\t71 72 73 74 75                                 \tqrstu\n"
        "\n"
        "< 0000:\t76 77 78 79 7a                                 \tvwxyz\n"
        "\n"
    )
    assert out.getvalue() == expected


def test_DebugStream_write():
    under = io.BytesIO()
    out = io.StringIO()

    stream = _DebugStream(under, out)

    stream.write(b"\x00\x01\x02")
    stream.flush()

    assert under.getvalue() == b"\x00\x01\x02"

    stream.write(b"\xc0\xc1\xc2")
    stream.flush()

    assert under.getvalue() == b"\x00\x01\x02\xc0\xc1\xc2"

    stream.flush_debug()

    expected = "> 0000:\t00 01 02 c0 c1 c2                              \t......\n\n"
    assert out.getvalue() == expected


def test_DebugStream_read_write():
    under = io.BytesIO(b"abcdefghijklmnopqrstuvwxyz")
    out = io.StringIO()
    stream = _DebugStream(under, out)

    res = stream.read(5)
    assert res == b"abcde"

    stream.write(b"xxxxx")
    stream.flush()

    assert under.getvalue() == b"abcdexxxxxklmnopqrstuvwxyz"

    res = stream.read(5)
    assert res == b"klmno"

    stream.write(b"xxxxx")
    stream.flush()

    assert under.getvalue() == b"abcdexxxxxklmnoxxxxxuvwxyz"

    stream.flush_debug()

    expected = (
        "< 0000:\t61 62 63 64 65 6b 6c 6d 6e 6f                  \tabcdeklmno\n"
        "\n"
        "> 0000:\t78 78 78 78 78 78 78 78 78 78                  \txxxxxxxxxx\n"
        "\n"
    )
    assert out.getvalue() == expected


def test_DebugStream_end_packet():
    under = io.BytesIO(b"abcdefghijklmnopqrstuvwxyz")
    out = io.StringIO()
    stream = _DebugStream(under, out)

    stream.read(5)
    stream.end_packet("read description", read=True, indent=" ")

    stream.write(b"xxxxx")
    stream.flush()
    stream.end_packet("write description", indent=" ")

    stream.read(5)
    stream.write(b"xxxxx")
    stream.flush()
    stream.end_packet("read/write combo for read", read=True, indent=" ")

    stream.read(5)
    stream.write(b"xxxxx")
    stream.flush()
    stream.end_packet("read/write combo for write", indent=" ")

    expected = (
        " < 0000:\t61 62 63 64 65                                 \tabcde\n"
        "\n"
        "< read description\n"
        "\n"
        "> write description\n"
        "\n"
        " > 0000:\t78 78 78 78 78                                 \txxxxx\n"
        "\n"
        " < 0000:\t6b 6c 6d 6e 6f                                 \tklmno\n"
        "\n"
        " > 0000:\t78 78 78 78 78                                 \txxxxx\n"
        "\n"
        "< read/write combo for read\n"
        "\n"
        "> read/write combo for write\n"
        "\n"
        " < 0000:\t75 76 77 78 79                                 \tuvwxy\n"
        "\n"
        " > 0000:\t78 78 78 78 78                                 \txxxxx\n"
        "\n"
    )
    assert out.getvalue() == expected
