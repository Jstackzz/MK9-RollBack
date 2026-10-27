"""Print the XEX2 header of an Xbox 360 executable: title id, version, base address,
entry point, compression/encryption, import libraries.

Usage: python xex_info.py default.xex
"""
import struct
import sys

HEADER_KEYS = {
    0x000002FF: "resource_info",
    0x000003FF: "base_file_format",
    0x00010001: "original_base_address",
    0x00010100: "entry_point",
    0x00010201: "image_base_address",
    0x000103FF: "import_libraries",
    0x00018002: "checksum_timestamp",
    0x00018102: "enabled_for_callcap",
    0x000183FF: "original_pe_name",
    0x000200FF: "static_libraries",
    0x00020104: "tls_info",
    0x00020200: "default_stack_size",
    0x00020301: "default_filesystem_cache_size",
    0x00020401: "default_heap_size",
    0x00030000: "system_flags",
    0x00040006: "execution_info",
    0x000400FF: "title_workspace_size",
    0x00040201: "game_ratings",
    0x00040310: "lan_key",
    0x000405FF: "xbox360_logo",
    0x000406FF: "multidisc_media_ids",
    0x000407FF: "alternate_title_ids",
    0x00040801: "additional_title_memory",
    0x00E10402: "exports_by_name",
}


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"XEX2":
        raise SystemExit("not an XEX2 file")
    module_flags, pe_offset, _, sec_offset, opt_count = struct.unpack_from(">IIIII", data, 4)
    print(f"module flags 0x{module_flags:08X}, PE data at 0x{pe_offset:X}, security info at 0x{sec_offset:X}")
    headers = {}
    for i in range(opt_count):
        key, value = struct.unpack_from(">II", data, 24 + i * 8)
        headers[key] = value
        print(f"  header 0x{key:08X} {HEADER_KEYS.get(key, '?'):32} 0x{value:08X}")
    sec_size = u32(data, sec_offset)
    image_size = u32(data, sec_offset + 4)
    load_address = u32(data, sec_offset + 0x110)
    print(f"security info size 0x{sec_size:X}, image size 0x{image_size:X}, load address 0x{load_address:08X}")
    if 0x00010100 in headers:
        print(f"entry point 0x{headers[0x00010100]:08X}")
    if 0x00040006 in headers:
        o = headers[0x00040006]
        media_id, version, base_version, title_id = struct.unpack_from(">IIII", data, o)
        platform, exe_type, disc, discs = struct.unpack_from(">BBBB", data, o + 16)
        v = lambda x: f"{x >> 28}.{(x >> 24) & 0xF}.{(x >> 8) & 0xFFFF}.{x & 0xFF}"
        print(f"title id {title_id:08X}, media id {media_id:08X}, version {v(version)}, base {v(base_version)}, disc {disc}/{discs}")
    if 0x000003FF in headers:
        o = headers[0x000003FF]
        size, enc, comp = struct.unpack_from(">IHH", data, o)
        print(f"base file format: encryption {enc} (1 = encrypted), compression {comp} (1 basic, 2 normal/LZX, 3 delta)")
        if comp == 2:
            print(f"  LZX window size 0x{u32(data, o + 8):X}")
    if 0x000183FF in headers:
        o = headers[0x000183FF]
        n = u32(data, o)
        print("original PE name:", data[o + 4:o + n].split(b"\0")[0].decode("latin-1"))
    if 0x000200FF in headers:
        o = headers[0x000200FF]
        n = u32(data, o)
        count = (n - 4) // 16
        libs = []
        for i in range(count):
            name = data[o + 4 + i * 16: o + 12 + i * 16].split(b"\0")[0].decode("latin-1")
            major, minor, build, qfe = struct.unpack_from(">HHHH", data, o + 12 + i * 16)
            libs.append(f"{name} {major}.{minor}.{build}.{qfe & 0x7FFF}")
        print("static libraries:", ", ".join(libs))
    if 0x000103FF in headers:
        o = headers[0x000103FF]
        size, str_size, lib_count = struct.unpack_from(">III", data, o)
        strings = data[o + 12: o + 12 + str_size].split(b"\0")
        names = [s.decode("latin-1") for s in strings if s]
        p = o + 12 + str_size
        print("import libraries:")
        for i in range(lib_count):
            lib_size = u32(data, p)
            name_index = struct.unpack_from(">H", data, p + 0x24)[0]
            record_count = struct.unpack_from(">H", data, p + 0x26)[0]
            ver = u32(data, p + 0x1C)
            print(f"  {names[name_index] if name_index < len(names) else '?':20} records {record_count:5}  version {ver >> 28}.{(ver >> 24) & 0xF}.{(ver >> 8) & 0xFFFF}.{ver & 0xFF}")
            p += lib_size
    if 0x00020104 in headers:
        o = headers[0x00020104]
        slots, raw, data_size, raw_size = struct.unpack_from(">IIII", data, o)
        print(f"TLS: {slots} slots, raw data 0x{raw:08X}, data size 0x{data_size:X}, raw size 0x{raw_size:X}")
    if 0x00020200 in headers:
        print(f"default stack size 0x{headers[0x00020200]:X}")
    if 0x00040201 in headers:
        pass


if __name__ == "__main__":
    main()
