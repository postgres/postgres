#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#

from construct import *

#
# TLS 1.3
#
# Most of the types below are transcribed from RFC 8446:
#
#     https://tools.ietf.org/html/rfc8446
#


def _Vector(size_field, element):
    return Prefixed(size_field, GreedyRange(element))


# Alerts

AlertLevel = Enum(
    Byte,
    warning=1,
    fatal=2,
)

AlertDescription = Enum(
    Byte,
    close_notify=0,
    unexpected_message=10,
    bad_record_mac=20,
    decryption_failed_RESERVED=21,
    record_overflow=22,
    decompression_failure=30,
    handshake_failure=40,
    no_certificate_RESERVED=41,
    bad_certificate=42,
    unsupported_certificate=43,
    certificate_revoked=44,
    certificate_expired=45,
    certificate_unknown=46,
    illegal_parameter=47,
    unknown_ca=48,
    access_denied=49,
    decode_error=50,
    decrypt_error=51,
    export_restriction_RESERVED=60,
    protocol_version=70,
    insufficient_security=71,
    internal_error=80,
    user_canceled=90,
    no_renegotiation=100,
    unsupported_extension=110,
)

Alert = Struct(
    "level" / AlertLevel,
    "description" / AlertDescription,
)


# Extensions

ExtensionType = Enum(
    Int16ub,
    server_name=0,
    max_fragment_length=1,
    status_request=5,
    supported_groups=10,
    signature_algorithms=13,
    use_srtp=14,
    heartbeat=15,
    application_layer_protocol_negotiation=16,
    signed_certificate_timestamp=18,
    client_certificate_type=19,
    server_certificate_type=20,
    padding=21,
    pre_shared_key=41,
    early_data=42,
    supported_versions=43,
    cookie=44,
    psk_key_exchange_modes=45,
    certificate_authorities=47,
    oid_filters=48,
    post_handshake_auth=49,
    signature_algorithms_cert=50,
    key_share=51,
)

Extension = Struct(
    "extension_type" / ExtensionType,
    "extension_data" / Prefixed(Int16ub, GreedyBytes),
)


# ClientHello


class _CipherSuiteAdapter(Adapter):
    class _hextuple(tuple):
        def __repr__(self):
            return f"(0x{self[0]:02X}, 0x{self[1]:02X})"

    def _encode(self, obj, context, path):
        return bytes(obj)

    def _decode(self, obj, context, path):
        assert len(obj) == 2
        return self._hextuple(obj)


ProtocolVersion = Hex(Int16ub)

Random = Hex(Bytes(32))

CipherSuite = _CipherSuiteAdapter(Byte[2])

ClientHello = Struct(
    "legacy_version" / ProtocolVersion,
    "random" / Random,
    "legacy_session_id" / Prefixed(Byte, Hex(GreedyBytes)),
    "cipher_suites" / _Vector(Int16ub, CipherSuite),
    "legacy_compression_methods" / Prefixed(Byte, GreedyBytes),
    "extensions" / _Vector(Int16ub, Extension),
)

# ServerHello

ServerHello = Struct(
    "legacy_version" / ProtocolVersion,
    "random" / Random,
    "legacy_session_id_echo" / Prefixed(Byte, Hex(GreedyBytes)),
    "cipher_suite" / CipherSuite,
    "legacy_compression_method" / Hex(Byte),
    "extensions" / _Vector(Int16ub, Extension),
)

# Handshake

HandshakeType = Enum(
    Byte,
    client_hello=1,
    server_hello=2,
    new_session_ticket=4,
    end_of_early_data=5,
    encrypted_extensions=8,
    certificate=11,
    certificate_request=13,
    certificate_verify=15,
    finished=20,
    key_update=24,
    message_hash=254,
)

Handshake = Struct(
    "msg_type" / HandshakeType,
    "length" / Int24ub,
    "payload"
    / Switch(
        this.msg_type,
        {
            HandshakeType.client_hello: ClientHello,
            HandshakeType.server_hello: ServerHello,
            # HandshakeType.end_of_early_data: EndOfEarlyData,
            # HandshakeType.encrypted_extensions: EncryptedExtensions,
            # HandshakeType.certificate_request: CertificateRequest,
            # HandshakeType.certificate: Certificate,
            # HandshakeType.certificate_verify: CertificateVerify,
            # HandshakeType.finished: Finished,
            # HandshakeType.new_session_ticket: NewSessionTicket,
            # HandshakeType.key_update: KeyUpdate,
        },
        default=FixedSized(this.length, GreedyBytes),
    ),
)

# Records

ContentType = Enum(
    Byte,
    invalid=0,
    change_cipher_spec=20,
    alert=21,
    handshake=22,
    application_data=23,
)

Plaintext = Struct(
    "type" / ContentType,
    "legacy_record_version" / ProtocolVersion,
    "length" / Int16ub,
    "fragment" / FixedSized(this.length, GreedyBytes),
)
