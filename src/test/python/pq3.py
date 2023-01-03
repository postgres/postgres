#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#

import contextlib
import getpass
import io
import os
import ssl
import sys
import textwrap

from construct import *

import tls


def protocol(major, minor):
    """
    Returns the protocol version, in integer format, corresponding to the given
    major and minor version numbers.
    """
    return (major << 16) | minor


# Startup

StringList = GreedyRange(NullTerminated(GreedyBytes))


class KeyValueAdapter(Adapter):
    """
    Turns a key-value store into a null-terminated list of null-terminated
    strings, as presented on the wire in the startup packet.
    """

    def _encode(self, obj, context, path):
        if isinstance(obj, list):
            return obj

        l = []

        for k, v in obj.items():
            if isinstance(k, str):
                k = k.encode("utf-8")
            l.append(k)

            if isinstance(v, str):
                v = v.encode("utf-8")
            l.append(v)

        l.append(b"")
        return l

    def _decode(self, obj, context, path):
        # TODO: turn a list back into a dict
        return obj


KeyValues = KeyValueAdapter(StringList)

_startup_payload = Switch(
    this.proto,
    {
        protocol(3, 0): KeyValues,
    },
    default=GreedyBytes,
)


def _default_protocol(this):
    try:
        if isinstance(this.payload, (list, dict)):
            return protocol(3, 0)
    except AttributeError:
        pass  # no payload passed during build

    return 0


def _startup_payload_len(this):
    """
    The payload field has a fixed size based on the length of the packet. But
    if the caller hasn't supplied an explicit length at build time, we have to
    build the payload to figure out how long it is, which requires us to know
    the length first... This function exists solely to break the cycle.
    """
    assert this._building, "_startup_payload_len() cannot be called during parsing"

    try:
        payload = this.payload
    except AttributeError:
        return 0  # no payload

    if isinstance(payload, bytes):
        # already serialized; just use the given length
        return len(payload)

    try:
        proto = this.proto
    except AttributeError:
        proto = _default_protocol(this)

    data = _startup_payload.build(payload, proto=proto)
    return len(data)


Startup = Struct(
    "len" / Default(Int32sb, lambda this: _startup_payload_len(this) + 8),
    "proto" / Default(Hex(Int32sb), _default_protocol),
    "payload" / FixedSized(this.len - 8, Default(_startup_payload, b"")),
)

# Pq3

# Adapted from construct.core.EnumIntegerString
class EnumNamedByte:
    def __init__(self, val, name):
        self._val = val
        self._name = name

    def __int__(self):
        return ord(self._val)

    def __str__(self):
        return "(enum) %s %r" % (self._name, self._val)

    def __repr__(self):
        return "EnumNamedByte(%r)" % self._val

    def __eq__(self, other):
        if isinstance(other, EnumNamedByte):
            other = other._val
        if not isinstance(other, bytes):
            return NotImplemented

        return self._val == other

    def __hash__(self):
        return hash(self._val)


# Adapted from construct.core.Enum
class ByteEnum(Adapter):
    def __init__(self, **mapping):
        super(ByteEnum, self).__init__(Byte)
        self.namemapping = {k: EnumNamedByte(v, k) for k, v in mapping.items()}
        self.decmapping = {v: EnumNamedByte(v, k) for k, v in mapping.items()}

    def __getattr__(self, name):
        if name in self.namemapping:
            return self.decmapping[self.namemapping[name]]
        raise AttributeError

    def _decode(self, obj, context, path):
        b = bytes([obj])
        try:
            return self.decmapping[b]
        except KeyError:
            return EnumNamedByte(b, "(unknown)")

    def _encode(self, obj, context, path):
        if isinstance(obj, int):
            return obj
        elif isinstance(obj, bytes):
            return ord(obj)
        return int(obj)


types = ByteEnum(
    ErrorResponse=b"E",
    ReadyForQuery=b"Z",
    Query=b"Q",
    EmptyQueryResponse=b"I",
    AuthnRequest=b"R",
    PasswordMessage=b"p",
    BackendKeyData=b"K",
    CommandComplete=b"C",
    ParameterStatus=b"S",
    DataRow=b"D",
    Terminate=b"X",
)


authn = Enum(
    Int32ub,
    OK=0,
    SASL=10,
    SASLContinue=11,
    SASLFinal=12,
)


_authn_body = Switch(
    this.type,
    {
        authn.OK: Terminated,
        authn.SASL: StringList,
    },
    default=GreedyBytes,
)


def _data_len(this):
    assert this._building, "_data_len() cannot be called during parsing"

    if not hasattr(this, "data") or this.data is None:
        return -1

    return len(this.data)


# The protocol reuses the PasswordMessage for several authentication response
# types, and there's no good way to figure out which is which without keeping
# state for the entire stream. So this is a separate Construct that can be
# explicitly parsed/built by code that knows it's needed.
SASLInitialResponse = Struct(
    "name" / NullTerminated(GreedyBytes),
    "len" / Default(Int32sb, lambda this: _data_len(this)),
    "data"
    / IfThenElse(
        # Allow tests to explicitly pass an incorrect length during testing, by
        # not enforcing a FixedSized during build. (The len calculation above
        # defaults to the correct size.)
        this._building,
        Optional(GreedyBytes),
        If(this.len != -1, Default(FixedSized(this.len, GreedyBytes), b"")),
    ),
    Terminated,  # make sure the entire response is consumed
)


_column = FocusedSeq(
    "data",
    "len" / Default(Int32sb, lambda this: _data_len(this)),
    "data" / If(this.len != -1, FixedSized(this.len, GreedyBytes)),
)


_payload_map = {
    types.ErrorResponse: Struct("fields" / StringList),
    types.ReadyForQuery: Struct("status" / Bytes(1)),
    types.Query: Struct("query" / NullTerminated(GreedyBytes)),
    types.EmptyQueryResponse: Terminated,
    types.AuthnRequest: Struct("type" / authn, "body" / Default(_authn_body, b"")),
    types.BackendKeyData: Struct("pid" / Int32ub, "key" / Hex(Int32ub)),
    types.CommandComplete: Struct("tag" / NullTerminated(GreedyBytes)),
    types.ParameterStatus: Struct(
        "name" / NullTerminated(GreedyBytes), "value" / NullTerminated(GreedyBytes)
    ),
    types.DataRow: Struct("columns" / Default(PrefixedArray(Int16sb, _column), b"")),
    types.Terminate: Terminated,
}


_payload = FocusedSeq(
    "_payload",
    "_payload"
    / Switch(
        this._.type,
        _payload_map,
        default=GreedyBytes,
    ),
    Terminated,  # make sure every payload consumes the entire packet
)


def _payload_len(this):
    """
    See _startup_payload_len() for an explanation.
    """
    assert this._building, "_payload_len() cannot be called during parsing"

    try:
        payload = this.payload
    except AttributeError:
        return 0  # no payload

    if isinstance(payload, bytes):
        # already serialized; just use the given length
        return len(payload)

    data = _payload.build(payload, type=this.type)
    return len(data)


Pq3 = Struct(
    "type" / types,
    "len" / Default(Int32ub, lambda this: _payload_len(this) + 4),
    "payload" / FixedSized(this.len - 4, Default(_payload, b"")),
)


# Environment


def pghost():
    return os.environ.get("PGHOST", default="localhost")


def pgport():
    return int(os.environ.get("PGPORT", default=5432))


def pguser():
    try:
        return os.environ["PGUSER"]
    except KeyError:
        return getpass.getuser()


def pgdatabase():
    return os.environ.get("PGDATABASE", default="postgres")


# Connections


def _hexdump_translation_map():
    """
    For hexdumps. Translates any unprintable or non-ASCII bytes into '.'.
    """
    input = bytearray()

    for i in range(128):
        c = chr(i)

        if not c.isprintable():
            input += bytes([i])

    input += bytes(range(128, 256))

    return bytes.maketrans(input, b"." * len(input))


class _DebugStream(object):
    """
    Wraps a file-like object and adds hexdumps of the read and write data. Call
    end_packet() on a _DebugStream to write the accumulated hexdumps to the
    output stream, along with the packet that was sent.
    """

    _translation_map = _hexdump_translation_map()

    def __init__(self, stream, out=sys.stdout):
        """
        Creates a new _DebugStream wrapping the given stream (which must have
        been created by wrap()). All attributes not provided by the _DebugStream
        are delegated to the wrapped stream. out is the text stream to which
        hexdumps are written.
        """
        self.raw = stream
        self._out = out
        self._rbuf = io.BytesIO()
        self._wbuf = io.BytesIO()

    def __getattr__(self, name):
        return getattr(self.raw, name)

    def __setattr__(self, name, value):
        if name in ("raw", "_out", "_rbuf", "_wbuf"):
            return object.__setattr__(self, name, value)

        setattr(self.raw, name, value)

    def read(self, *args, **kwargs):
        buf = self.raw.read(*args, **kwargs)

        self._rbuf.write(buf)
        return buf

    def write(self, b):
        self._wbuf.write(b)
        return self.raw.write(b)

    def recv(self, *args):
        buf = self.raw.recv(*args)

        self._rbuf.write(buf)
        return buf

    def _flush(self, buf, prefix):
        width = 16
        hexwidth = width * 3 - 1

        count = 0
        buf.seek(0)

        while True:
            line = buf.read(16)

            if not line:
                if count:
                    self._out.write("\n")  # separate the output block with a newline
                return

            self._out.write("%s %04X:\t" % (prefix, count))
            self._out.write("%*s\t" % (-hexwidth, line.hex(" ")))
            self._out.write(line.translate(self._translation_map).decode("ascii"))
            self._out.write("\n")

            count += 16

    def print_debug(self, obj, *, prefix=""):
        contents = ""
        if obj is not None:
            contents = str(obj)

        for line in contents.splitlines():
            self._out.write("%s%s\n" % (prefix, line))

        self._out.write("\n")

    def flush_debug(self, *, prefix=""):
        self._flush(self._rbuf, prefix + "<")
        self._rbuf = io.BytesIO()

        self._flush(self._wbuf, prefix + ">")
        self._wbuf = io.BytesIO()

    def end_packet(self, pkt, *, read=False, prefix="", indent="  "):
        """
        Marks the end of a logical "packet" of data. A string representation of
        pkt will be printed, and the debug buffers will be flushed with an
        indent. All lines can be optionally prefixed.

        If read is True, the packet representation is written after the debug
        buffers; otherwise the default of False (meaning write) causes the
        packet representation to be dumped first. This is meant to capture the
        logical flow of layer translation.
        """
        write = not read

        if write:
            self.print_debug(pkt, prefix=prefix + "> ")

        self.flush_debug(prefix=prefix + indent)

        if read:
            self.print_debug(pkt, prefix=prefix + "< ")


@contextlib.contextmanager
def wrap(socket, *, debug_stream=None):
    """
    Transforms a raw socket into a connection that can be used for Construct
    building and parsing. The return value is a context manager and can be used
    in a with statement.
    """
    # It is critical that buffering be disabled here, so that we can still
    # manipulate the raw socket without desyncing the stream.
    with socket.makefile("rwb", buffering=0) as sfile:
        # Expose the original socket's recv() on the SocketIO object we return.
        def recv(self, *args):
            return socket.recv(*args)

        sfile.recv = recv.__get__(sfile)

        conn = sfile
        if debug_stream:
            conn = _DebugStream(conn, debug_stream)

        try:
            yield conn
        finally:
            if debug_stream:
                conn.flush_debug(prefix="? ")


def _send(stream, cls, obj):
    debugging = hasattr(stream, "flush_debug")
    out = io.BytesIO()

    # Ideally we would build directly to the passed stream, but because we need
    # to reparse the generated output for the debugging case, build to an
    # intermediate BytesIO and send it instead.
    cls.build_stream(obj, out)
    buf = out.getvalue()

    stream.write(buf)
    if debugging:
        pkt = cls.parse(buf)
        stream.end_packet(pkt)

    stream.flush()


def send(stream, packet_type, payload_data=None, **payloadkw):
    """
    Sends a packet on the given pq3 connection. type is the pq3.types member
    that should be assigned to the packet. If payload_data is given, it will be
    used as the packet payload; otherwise the key/value pairs in payloadkw will
    be the payload contents.
    """
    data = payloadkw

    if payload_data is not None:
        if payloadkw:
            raise ValueError(
                "payload_data and payload keywords may not be used simultaneously"
            )

        data = payload_data

    _send(stream, Pq3, dict(type=packet_type, payload=data))


def send_startup(stream, proto=None, **kwargs):
    """
    Sends a startup packet on the given pq3 connection. In most cases you should
    use the handshake functions instead, which will do this for you.

    By default, a protocol version 3 packet will be sent. This can be overridden
    with the proto parameter.
    """
    pkt = {}

    if proto is not None:
        pkt["proto"] = proto
    if kwargs:
        pkt["payload"] = kwargs

    _send(stream, Startup, pkt)


def recv1(stream, *, cls=Pq3):
    """
    Receives a single pq3 packet from the given stream and returns it.
    """
    resp = cls.parse_stream(stream)

    debugging = hasattr(stream, "flush_debug")
    if debugging:
        stream.end_packet(resp, read=True)

    return resp


def handshake(stream, **kwargs):
    """
    Performs a libpq v3 startup handshake. kwargs should contain the key/value
    parameters to send to the server in the startup packet.
    """
    # Send our startup parameters.
    send_startup(stream, **kwargs)

    # Receive and dump packets until the server indicates it's ready for our
    # first query.
    while True:
        resp = recv1(stream)
        if resp is None:
            raise RuntimeError("server closed connection during handshake")

        if resp.type == types.ReadyForQuery:
            return
        elif resp.type == types.ErrorResponse:
            raise RuntimeError(
                f"received error response from peer: {resp.payload.fields!r}"
            )


# TLS


class _TLSStream(object):
    """
    A file-like object that performs TLS encryption/decryption on a wrapped
    stream. Differs from ssl.SSLSocket in that we have full visibility and
    control over the TLS layer.
    """

    def __init__(self, stream, context):
        self._stream = stream
        self._debugging = hasattr(stream, "flush_debug")

        self._in = ssl.MemoryBIO()
        self._out = ssl.MemoryBIO()
        self._ssl = context.wrap_bio(self._in, self._out)

    def handshake(self):
        try:
            self._pump(lambda: self._ssl.do_handshake())
        finally:
            self._flush_debug(prefix="? ")

    def read(self, *args):
        return self._pump(lambda: self._ssl.read(*args))

    def write(self, *args):
        return self._pump(lambda: self._ssl.write(*args))

    def _decode(self, buf):
        """
        Attempts to decode a buffer of TLS data into a packet representation
        that can be printed.

        TODO: handle buffers (and record fragments) that don't align with packet
        boundaries.
        """
        end = len(buf)
        bio = io.BytesIO(buf)

        ret = io.StringIO()

        while bio.tell() < end:
            record = tls.Plaintext.parse_stream(bio)

            if ret.tell() > 0:
                ret.write("\n")
            ret.write("[Record] ")
            ret.write(str(record))
            ret.write("\n")

            if record.type == tls.ContentType.handshake:
                record_cls = tls.Handshake
            else:
                continue

            innerlen = len(record.fragment)
            inner = io.BytesIO(record.fragment)

            while inner.tell() < innerlen:
                msg = record_cls.parse_stream(inner)

                indented = "[Message] " + str(msg)
                indented = textwrap.indent(indented, "    ")

                ret.write("\n")
                ret.write(indented)
                ret.write("\n")

        return ret.getvalue()

    def flush(self):
        if not self._out.pending:
            self._stream.flush()
            return

        buf = self._out.read()
        self._stream.write(buf)

        if self._debugging:
            pkt = self._decode(buf)
            self._stream.end_packet(pkt, prefix="  ")

        self._stream.flush()

    def _pump(self, operation):
        while True:
            try:
                return operation()
            except (ssl.SSLWantReadError, ssl.SSLWantWriteError) as e:
                want = e
            self._read_write(want)

    def _recv(self, maxsize):
        buf = self._stream.recv(4096)
        if not buf:
            self._in.write_eof()
            return

        self._in.write(buf)

        if not self._debugging:
            return

        pkt = self._decode(buf)
        self._stream.end_packet(pkt, read=True, prefix="  ")

    def _read_write(self, want):
        # XXX This needs work. So many corner cases yet to handle. For one,
        # doing blocking writes in flush may lead to distributed deadlock if the
        # peer is already blocking on its writes.

        if isinstance(want, ssl.SSLWantWriteError):
            assert self._out.pending, "SSL backend wants write without data"

        self.flush()

        if isinstance(want, ssl.SSLWantReadError):
            self._recv(4096)

    def _flush_debug(self, prefix):
        if not self._debugging:
            return

        self._stream.flush_debug(prefix=prefix)


@contextlib.contextmanager
def tls_handshake(stream, context):
    """
    Performs a TLS handshake over the given stream (which must have been created
    via a call to wrap()), and returns a new stream which transparently tunnels
    data over the TLS connection.

    If the passed stream has debugging enabled, the returned stream will also
    have debugging, using the same output IO.
    """
    debugging = hasattr(stream, "flush_debug")

    # Send our startup parameters.
    send_startup(stream, proto=protocol(1234, 5679))

    # Look at the SSL response.
    resp = stream.read(1)
    if debugging:
        stream.flush_debug(prefix="  ")

    if resp == b"N":
        raise RuntimeError("server does not support SSLRequest")
    if resp != b"S":
        raise RuntimeError(f"unexpected response of type {resp!r} during TLS startup")

    tls = _TLSStream(stream, context)
    tls.handshake()

    if debugging:
        tls = _DebugStream(tls, stream._out)

    try:
        yield tls
        # TODO: teardown/unwrap the connection?
    finally:
        if debugging:
            tls.flush_debug(prefix="? ")
