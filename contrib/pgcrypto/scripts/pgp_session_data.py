#!/usr/bin/python
# -*- coding: utf-8 -*-
#
# Generate PGP data to check the session key length of the input data provided
# to pgp_pub_decrypt_bytea().
#
# First, the crafted data is generated from valid RSA data, freshly generated
# by this script each time it is run, see generate_rsa_keypair().
# Second, the crafted PGP data is built, see build_message_data() and
# build_key_data().  Finally, the resulting SQL script is generated.
#
# This script generates in stdout the SQL file that is used in the regression
# tests of pgcrypto.  The following command can be used to regenerate the file
# which should never be manually manipulated:
# python3 scripts/pgp_session_data.py > sql/pgp-pubkey-session.sql

import os
import re
import struct
import secrets
import sys
import time

# pwn for binary manipulation (p32, p64)
from pwn import *

# Cryptographic libraries, to craft the PGP data.
from Crypto.Cipher import AES
from Crypto.PublicKey import RSA
from Crypto.Util.number import inverse

# AES key used for session key encryption (16 bytes for AES-128)
AES_KEY = b'\x01' * 16

def generate_rsa_keypair(key_size: int = 2048) -> dict:
    """
    Generate a fresh RSA key pair.

    The generated key includes all components needed for PGP operations:
    - n: public modulus (p * q)
    - e: public exponent (typically 65537)
    - d: private exponent (e^-1 mod phi(n))
    - p, q: prime factors of n
    - u: coefficient (p^-1 mod q) for CRT optimization

    The caller can pass the wanted key size in input, for a default of 2048
    bytes.  This function returns the RSA key components, after performing
    some validation on them.
    """

    start_time = time.time()

    # Generate RSA key
    key = RSA.generate(key_size)

    # Extract all key components
    rsa_components = {
        'n': key.n,      # Public modulus (p * q)
        'e': key.e,      # Public exponent (typically 65537)
        'd': key.d,      # Private exponent (e^-1 mod phi(n))
        'p': key.p,      # First prime factor
        'q': key.q,      # Second prime factor
        'u': inverse(key.p, key.q)  # Coefficient for CRT: p^-1 mod q
    }

    # Validate key components for correctness
    validate_rsa_key(rsa_components)

    return rsa_components

def validate_rsa_key(rsa: dict) -> None:
    """
    Validate a generated RSA key.

    This function performs basic validation to ensure the RSA key is properly
    constructed and all components are consistent, at least mathematically.

    Validations performed:
    1. n = p * q (modulus is product of primes)
    2. gcd(e, phi(n)) = 1 (public exponent is coprime to phi(n))
    3. (d * e) mod(phi(n)) = 1 (private exponent is multiplicative inverse)
    4. (u * p) (mod q) = 1 (coefficient is correct for CRT)
    """

    n, e, d, p, q, u = rsa['n'], rsa['e'], rsa['d'], rsa['p'], rsa['q'], rsa['u']

    # Check that n = p * q
    if n != p * q:
        raise ValueError("RSA validation failed: n <> p * q")

    # Check that p and q are different
    if p == q:
        raise ValueError("RSA validation failed: p = q (not allowed)")

    # Calculate phi(n) = (p-1)(q-1)
    phi_n = (p - 1) * (q - 1)

    # Check that gcd(e, phi(n)) = 1
    def gcd(a, b):
        while b:
            a, b = b, a % b
        return a

    if gcd(e, phi_n) != 1:
        raise ValueError("RSA validation failed: gcd(e, phi(n)) <> 1")

    # Check that (d * e) mod(phi(n)) = 1
    if (d * e) % phi_n != 1:
        raise ValueError("RSA validation failed: d * e <> 1 (mod phi(n))")

    # Check that (u * p) (mod q) = 1
    if (u * p) % q != 1:
        raise ValueError("RSA validation failed: u * p <> 1 (mod q)")

def mpi_encode(x: int) -> bytes:
    """
    Encode an integer as an OpenPGP Multi-Precision Integer (MPI).

    Format (RFC 4880, Section 3.2):
    - 2 bytes: bit length of the integer (big-endian)
    - N bytes: the integer in big-endian format

    This is used to encode RSA key components (n, e, d, p, q, u) in PGP
    packets.

    The integer to encode is given in input, returning an MPI-encoded
    integer.

    For example:
        mpi_encode(65537) -> b'\x00\x11\x01\x00\x01'
        (17 bits, value 0x010001)
    """
    if x < 0:
        raise ValueError("MPI cannot encode negative integers")

    if x == 0:
        # Special case: zero has 0 bits and empty magnitude
        bits = 0
        mag = b""
    else:
        # Calculate bit length and convert to bytes
        bits = x.bit_length()
        mag = x.to_bytes((bits + 7) // 8, 'big')

    # Pack: 2-byte bit length + magnitude bytes
    return struct.pack('>H', bits) + mag

def new_packet(tag: int, payload: bytes) -> bytes:
    """
    Create a new OpenPGP packet with a proper header.

    OpenPGP packet format (RFC 4880, Section 4.2):
    - New packet format: 0xC0 | tag
    - Length encoding depends on payload size:
      * 0-191: single byte
      * 192-8383: two bytes (192 + ((length - 192) >> 8), (length - 192) & 0xFF)
      * 8384+: five bytes (0xFF + 4-byte big-endian length)

    The packet is built from a "tag" (1-63) and some "payload" data.  The
    result generated is a complete OpenPGP packet.

    For example:
        new_packet(1, b'data') -> b'\xC1\x04data'
        (Tag 1, length 4, payload 'data')
    """
    # New packet format: set bit 7 and 6, clear bit 5, tag in bits 0-5
    first = 0xC0 | (tag & 0x3F)
    ln = len(payload)

    # Encode length according to OpenPGP specification
    if ln <= 191:
        # Single byte length for small packets
        llen = bytes([ln])
    elif ln <= 8383:
        # Two-byte length for medium packets
        ln2 = ln - 192
        llen = bytes([192 + (ln2 >> 8), ln2 & 0xFF])
    else:
        # Five-byte length for large packets
        llen = bytes([255]) + struct.pack('>I', ln)

    return bytes([first]) + llen + payload

def build_key_data(rsa: dict) -> bytes:
    """
    Build the key data, containing an RSA private key.

    The RSA contents should have been generated previously.

    Format (see RFC 4880, Section 5.5.3):
    - 1 byte: version (4)
    - 4 bytes: creation time (current Unix timestamp)
    - 1 byte: public key algorithm (2 = RSA encrypt)
    - MPI: RSA public modulus n
    - MPI: RSA public exponent e
    - 1 byte: string-to-key usage (0 = no encryption)
    - MPI: RSA private exponent d
    - MPI: RSA prime p
    - MPI: RSA prime q
    - MPI: RSA coefficient u = p^-1 mod q
    - 2 bytes: checksum of private key material

    This function takes a set of RSA key components in input (n, e, d, p, q, u)
    and returns a secret key packet.
    """

    # Public key portion
    ver = bytes([4])                           # Version 4 key
    ctime = struct.pack('>I', int(time.time())) # Current Unix timestamp
    algo = bytes([2])                          # RSA encrypt algorithm
    n_mpi = mpi_encode(rsa['n'])               # Public modulus
    e_mpi = mpi_encode(rsa['e'])               # Public exponent
    pub = ver + ctime + algo + n_mpi + e_mpi

    # Private key portion
    hide_type = bytes([0])              # No string-to-key encryption
    d_mpi = mpi_encode(rsa['d'])        # Private exponent
    p_mpi = mpi_encode(rsa['p'])        # Prime p
    q_mpi = mpi_encode(rsa['q'])        # Prime q
    u_mpi = mpi_encode(rsa['u'])        # Coefficient u = p^-1 mod q

    # Calculate checksum of private key material (simple sum mod 65536)
    private_data = d_mpi + p_mpi + q_mpi + u_mpi
    cksum = sum(private_data) & 0xFFFF

    secret = hide_type + private_data + struct.pack('>H', cksum)
    payload = pub + secret

    return new_packet(7, payload)

def pgp_cfb_encrypt_resync(key, plaintext):
    """
    Implement OpenPGP CFB mode with resync.

    OpenPGP CFB mode is a variant of standard CFB with a resync operation
    after the first two blocks.

    Algorithm (RFC 4880, Section 13.9):
    1. Block 1: FR=zeros, encrypt full block_size bytes
    2. Block 2: FR=block1, encrypt only 2 bytes
    3. Resync: FR = block1[2:] + block2
    4. Remaining blocks: standard CFB mode

    This function uses the following arguments:
    - key: AES encryption key (16 bytes for AES-128)
    - plaintext: Data to encrypt
    """
    block_size = 16  # AES block size
    cipher = AES.new(key[:16], AES.MODE_ECB)  # Use ECB for manual CFB
    ciphertext = b''

    # Block 1: FR=zeros, encrypt full 16 bytes
    FR = b'\x00' * block_size
    FRE = cipher.encrypt(FR)  # Encrypt the feedback register
    block1 = bytes(a ^ b for a, b in zip(FRE, plaintext[0:16]))
    ciphertext += block1

    # Block 2: FR=block1, encrypt only 2 bytes
    FR = block1
    FRE = cipher.encrypt(FR)
    block2 = bytes(a ^ b for a, b in zip(FRE[0:2], plaintext[16:18]))
    ciphertext += block2

    # Resync: FR = block1[2:16] + block2[0:2]
    # This is the key difference from standard CFB mode
    FR = block1[2:] + block2

    # Block 3+: Continue with standard CFB mode
    pos = 18
    while pos < len(plaintext):
        FRE = cipher.encrypt(FR)
        chunk_len = min(block_size, len(plaintext) - pos)
        chunk = plaintext[pos:pos+chunk_len]
        enc_chunk = bytes(a ^ b for a, b in zip(FRE[:chunk_len], chunk))
        ciphertext += enc_chunk

        # Update feedback register for next iteration
        if chunk_len == block_size:
            FR = enc_chunk
        else:
            # Partial block: pad with old FR bytes
            FR = enc_chunk + FR[chunk_len:]
        pos += chunk_len

    return ciphertext

def build_literal_data_packet(data: bytes) -> bytes:
    """
    Build a literal data packet containing a message.

    Format (RFC 4880, Section 5.9):
    - 1 byte: data format ('b' = binary, 't' = text, 'u' = UTF-8 text)
    - 1 byte: filename length (0 = no filename)
    - N bytes: filename (empty in this case)
    - 4 bytes: date (current Unix timestamp)
    - M bytes: literal data

    The data used to build the packet is given in input, with the generated
    result returned.
    """
    body = bytes([
        ord('b'),                              # Binary data format
        0,                                     # Filename length (0 = no filename)
    ]) + struct.pack('>I', int(time.time())) + data  # Current timestamp + data

    return new_packet(11, body)

def build_symenc_data_packet(sess_key: bytes, cipher_algo: int, payload: bytes) -> bytes:
    """
    Build a symmetrically-encrypted data packet using AES-128-CFB.

    This packet contains encrypted data using the session key. The format
    includes a random prefix, for security (see RFC 4880, Section 5.7).

    Packet structure:
    - Random prefix (block_size bytes)
    - Prefix repeat (last 2 bytes of prefix repeated)
    - Encrypted literal data packet

    This function uses the following set of arguments:
    - sess_key: Session key for encryption
    - cipher_algo: Cipher algorithm identifier (7 = AES-128)
    - payload: Data to encrypt (wrapped in literal data packet)
    """
    block_size = 16  # AES-128 block size
    key = sess_key[:16]  # Use first 16 bytes for AES-128

    # Create random prefix + repeat last 2 bytes (total 18 bytes)
    # This is required by OpenPGP for integrity checking
    prefix_random = secrets.token_bytes(block_size)
    prefix = prefix_random + prefix_random[-2:]  # 18 bytes total

    # Wrap payload in literal data packet
    literal_pkt = build_literal_data_packet(payload)

    # Plaintext = prefix + literal data packet
    plaintext = prefix + literal_pkt

    # Encrypt using OpenPGP CFB mode with resync
    ciphertext = pgp_cfb_encrypt_resync(key, plaintext)

    return new_packet(9, ciphertext)

def build_tag1_packet(rsa: dict, sess_key: bytes) -> bytes:
    """
    Build a public-key encrypted key.

    This is a very important function, as it is able to create the packet
    triggering the overflow check.  This function can also be used to create
    "legit" packet data.

    Format (RFC 4880, Section 5.1):
    - 1 byte: version (3)
    - 8 bytes: key ID (0 = any key accepted)
    - 1 byte: public key algorithm (2 = RSA encrypt)
    - MPI: RSA-encrypted session key

    This uses in arguments the generated RSA key pair, and the session key
    to encrypt.  The latter is manipulated to trigger the overflow.

    This function returns a complete packet encrypted by a session key.
    """

    # Calculate RSA modulus size in bytes
    n_bytes = (rsa['n'].bit_length() + 7) // 8

    # Session key message format:
    # - 1 byte: symmetric cipher algorithm (7 = AES-128)
    # - N bytes: session key
    # - 2 bytes: checksum (simple sum of session key bytes)
    algo_byte = bytes([7])  # AES-128 algorithm identifier
    cksum = sum(sess_key) & 0xFFFF  # 16-bit checksum
    M = algo_byte + sess_key + struct.pack('>H', cksum)

    # PKCS#1 v1.5 padding construction
    # Format: 0x02 || PS || 0x00 || M
    # Total padded message must be exactly n_bytes long.
    total_len = n_bytes  # Total length must equal modulus size in bytes
    ps_len = total_len - len(M) - 2  # Subtract 2 for 0x02 and 0x00 bytes

    if ps_len < 8:
        raise ValueError(f"Padding string too short ({ps_len} bytes); need at least 8 bytes. "
                        f"Message length: {len(M)}, Modulus size: {n_bytes} bytes")

    # Create padding string with *ALL* bytes being 0xFF (no zero separator!)
    PS = bytes([0xFF]) * ps_len

    # Construct the complete padded message
    # Normal PKCS#1 v1.5 padding: 0x02 || PS || 0x00 || M
    padded = bytes([0x02]) + PS + bytes([0x00]) + M

    # Verify padding construction
    if len(padded) != n_bytes:
        raise ValueError(f"Padded message length ({len(padded)}) doesn't match RSA modulus size ({n_bytes})")

    # Convert padded message to integer and encrypt with RSA
    m_int = int.from_bytes(padded, 'big')

    # Ensure message is smaller than modulus (required for RSA)
    if m_int >= rsa['n']:
        raise ValueError("Padded message is larger than RSA modulus")

    # RSA encryption: c = m^e mod n
    c_int = pow(m_int, rsa['e'], rsa['n'])

    # Encode encrypted result as MPI
    c_mpi = mpi_encode(c_int)

    # Build complete packet
    ver = bytes([3])           # Version 3 packet
    key_id = b"\x00" * 8      # Key ID (0 = any key accepted)
    algo = bytes([2])         # RSA encrypt algorithm
    payload = ver + key_id + algo + c_mpi

    return new_packet(1, payload)

def build_message_data(rsa: dict) -> bytes:
    """
    This function creates a crafted message, with a long session key
    length.

    This takes in input the RSA key components generated previously,
    returning a concatenated set of PGP packets crafted for the purpose
    of this test.
    """

    # Base prefix for session key (AES key + padding + size).
    # Note that the crafted size is the important part for this test.
    prefix = AES_KEY + b"\x00" * 16 + p32(0x10)

    # Build encrypted data packet, legit.
    sedata = build_symenc_data_packet(AES_KEY, cipher_algo=7, payload=b"\x0a\x00")

    # Build multiple packets
    packets = [
        # First packet, legit.
        build_tag1_packet(rsa, prefix),

        # Encrypted data packet, legit.
        sedata,

        # Second packet: information payload.
        #
        # This packet contains a longer-crafted session key, able to trigger
        # the overflow check in pgcrypto.  This is the critical part, and
        # and you are right to pay a lot of attention here if you are
        # reading this code.
        build_tag1_packet(rsa, prefix)
    ]

    return b"".join(packets)

def main():
    # Default key size.
    # This number can be set to a higher number if wanted, like 4096.  We
    # just do not need to do that here.
    key_size = 2048

    # Generate fresh RSA key pair
    rsa = generate_rsa_keypair(key_size)

    # Generate the message data.
    print("### Building message data", file=sys.stderr)
    message_data = build_message_data(rsa)

    # Build the key containing the RSA private key
    print("### Building key data", file=sys.stderr)
    key_data = build_key_data(rsa)

    # Convert to hexadecimal, for the bytea used in the SQL file.
    message_data = message_data.hex()
    key_data = key_data.hex()

    # Split each value into lines of 72 characters, for readability.
    message_data = re.sub("(.{72})", "\\1\n", message_data, 0, re.DOTALL)
    key_data = re.sub("(.{72})", "\\1\n", key_data, 0, re.DOTALL)

    # Get the script filename for documentation
    file_basename = os.path.basename(__file__)

    # Output the SQL test case
    print(f'''-- Test for overflow with session key at decrypt.
-- Data automatically generated by scripts/{file_basename}.
-- See this file for details explaining how this data is generated.
SELECT pgp_pub_decrypt_bytea(
'\\x{message_data}'::bytea,
'\\x{key_data}'::bytea);''',
          file=sys.stdout)

if __name__ == "__main__":
    main()
