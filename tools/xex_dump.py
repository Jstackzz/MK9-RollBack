"""Decrypt and decompress an XEX2 (basic or no compression) into its loaded PE image.

Usage: python xex_dump.py default.xex image.bin
The output is the image as mapped at its load address (0x82000000 for MK9), for Ghidra and
for scanning; ReXGlue reads the .xex itself.
"""
import struct
import sys

from Crypto.Cipher import AES

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def main():
    data = open(sys.argv[1], "rb").read()
    _, pe_offset, _, sec_offset, opt_count = struct.unpack_from(">IIIII", data, 4)
    headers = dict(struct.unpack_from(">II", data, 24 + i * 8) for i in range(opt_count))
    image_size = u32(data, sec_offset + 4)
    fmt = headers[0x000003FF]
    info_size, encryption, compression = struct.unpack_from(">IHH", data, fmt)
    payload = data[pe_offset:]
    if encryption:
        enc_key = data[sec_offset + 0x150: sec_offset + 0x160]
        for name, key in (("retail", RETAIL_KEY), ("devkit", DEVKIT_KEY)):
            session = AES.new(key, AES.MODE_ECB).decrypt(enc_key)
            n = len(payload) - len(payload) % 16
            plain = AES.new(session, AES.MODE_CBC, iv=bytes(16)).decrypt(payload[:n])
            if compression == 1:
                # basic compression: data then zeros, the first block starts with the PE's MZ
                if plain[:2] == b"MZ":
                    print(f"decrypted with the {name} key")
                    payload = plain
                    break
            else:
                payload = plain
                break
        else:
            raise SystemExit("could not decrypt: no key gave an MZ header")
    if compression == 1:
        out = bytearray()
        pos = 0
        for i in range((info_size - 8) // 8):
            data_size, zero_size = struct.unpack_from(">II", data, fmt + 8 + i * 8)
            out += payload[pos:pos + data_size]
            out += bytes(zero_size)
            pos += data_size
    elif compression == 0:
        out = bytearray(payload)
    else:
        raise SystemExit(f"compression {compression} (LZX/delta) not supported by this tool")
    if len(out) < image_size:
        out += bytes(image_size - len(out))
    open(sys.argv[2], "wb").write(out[:image_size])
    print(f"wrote {sys.argv[2]}: 0x{image_size:X} bytes, starts {bytes(out[:2])}")


if __name__ == "__main__":
    main()
