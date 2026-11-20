"""Replay a teach-and-solve workflow with the shipped Win64 executable on Linux x64.

Win32 APIs are mocked. The executable and its native code are the actual deliverable.
"""
import ctypes
import mmap
from pathlib import Path
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
NAMES = ("teach and solve two moved labels", "manual mouse stop",
         "teammate clears a label", "focus loss", "zero jitter",
         "injected flag without tag", "tag without injected flag", "saved lessons reload")


def value(fmt, data, at):
    return struct.unpack_from("<" + fmt, data, at)[0]


def as_cstr(data, at):
    return data[at:data.find(b"\0", at)]


def run_test(path, shim, scenario):
    pe = path.read_bytes()
    coff = value("I", pe, 0x3C) + 4
    opt = coff + 20
    assert pe[coff - 4:coff] == b"PE\0\0" and value("H", pe, coff) == 0x8664
    assert value("H", pe, opt) == 0x20B and value("H", pe, opt + 68) == 2
    sec = opt + value("H", pe, coff + 16)
    count = value("H", pe, coff + 2)
    image_size = value("I", pe, opt + 56)
    image_base = value("Q", pe, opt + 24)
    entry = value("I", pe, opt + 16)
    with mmap.mmap(-1, image_size, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC) as image:
        anchor = (ctypes.c_ubyte * 1).from_buffer(image)
        base = ctypes.addressof(anchor)
        del anchor
        for i in range(count):
            at = sec + i * 40
            _, addr, size, source = struct.unpack_from("<IIII", pe, at + 8)
            if size and source:
                image[addr:addr + size] = pe[source:source + size]

        reloc_dir, reloc_len = struct.unpack_from("<II", pe, opt + 112 + 8 * 5)
        pos = reloc_dir
        while pos < reloc_dir + reloc_len:
            page, length = struct.unpack_from("<II", image, pos)
            assert length >= 8
            for i in range(pos + 8, pos + length, 2):
                desc = value("H", image, i)
                code, offset = desc >> 12, desc & 0xFFF
                if code == 10:
                    where = page + offset
                    v = value("Q", image, where)
                    struct.pack_into("<Q", image, where, v + base - image_base)
                else:
                    assert code == 0
            pos += length

        pos = value("I", pe, opt + 112 + 8)
        names = []
        while any(image[pos:pos + 20]):
            lookup, _, _, name, first = struct.unpack_from("<IIIII", image, pos)
            names.append(as_cstr(image, name))
            i = 0
            while entry_import := value("Q", image, lookup + 8 * i):
                symbol = as_cstr(image, entry_import + 2)
                struct.pack_into("<Q", image, first + 8 * i, shim.resolve_for_test(symbol))
                i += 1
            pos += 20
        assert set(names) == {b"kernel32.dll", b"user32.dll", b"gdi32.dll"}
        assert shim.run_flow(base + entry, scenario), f"{path.name}, flow {scenario} failed"
        print(f"{path.name}: {NAMES[scenario]} passed through mocked Win32 calls")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        bridge = str(Path(directory) / "pe_startup.so")
        subprocess.run(["gcc", "-O2", "-fPIC", "-shared", "-o", bridge, str(HERE / "pe_flow.c")], check=True)
        shim = ctypes.CDLL(bridge)
        shim.resolve_for_test.argtypes = [ctypes.c_char_p]
        shim.resolve_for_test.restype = ctypes.c_size_t
        shim.run_flow.argtypes = [ctypes.c_void_p, ctypes.c_int]
        shim.run_flow.restype = ctypes.c_int
        updated = HERE.parent / "AutoDS.exe"
        for scenario in range(len(NAMES)):
            run_test(updated, shim, scenario)
