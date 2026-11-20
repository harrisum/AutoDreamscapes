"""Build a Win64 PE with GCC and GNU binutils, without shipping a runtime.

The Linux compiler emits ELF relocatable objects with the Microsoft ABI. This
small converter transfers their allocatable sections and link relocations to
AMD64 COFF. GNU ld handles the normal PE import table and final linking.
"""
import argparse
import collections
from pathlib import Path
import re
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCES = ("app.c", "desktop.c", "timing.c", "matcher.c", "lessons.c", "runtime.c")


def unpack(fmt, data, offset=0):
    return struct.unpack_from("<" + fmt, data, offset)


def pack(fmt, *values):
    return struct.pack("<" + fmt, *values)


def align(offset, alignment):
    return (offset + alignment - 1) & -alignment


def c_string(blob, offset):
    return blob[offset:blob.index(b"\0", offset)]


def convert(source, destination):
    elf = source.read_bytes()
    header = unpack("16sHHIQQQIHHHHHH", elf)
    assert header[0].startswith(b"\x7fELF\x02\x01") and header[1:3] == (1, 62)
    offset, entry_size, count = header[6], header[11], header[12]
    elf_sections = [unpack("IIQQQQIIQQ", elf, offset + i * entry_size) for i in range(count)]
    names = (b".text", b".rdata", b".data", b".bss")
    flags = (0x60500020, 0x40500040, 0xC0500040, 0xC0500080)
    sections = [bytearray() for _ in names]
    moves = {}
    for i, (_, kind, attrs, _, start, size, _, _, alignment, _) in enumerate(elf_sections):
        if not attrs & 2 or not size:
            continue
        if kind == 8:
            section = 3
        elif attrs & 4:
            section = 0
        elif attrs & 1:
            section = 2
        else:
            section = 1
        dest = sections[section]
        base = align(len(dest), max(1, alignment))
        dest.extend(b"\0" * (base - len(dest)))
        dest.extend(b"\0" * size if kind == 8 else elf[start:start + size])
        moves[i] = section, base

    strings = bytearray(b"\0\0\0\0")
    coff_symbols = []
    symbol_indices = {}

    def transfer_symbol(sym_section, index):
        key = sym_section, index
        if key in symbol_indices:
            return symbol_indices[key]
        _, _, _, _, start, _, strtab, _, _, stride = elf_sections[sym_section]
        str_start = elf_sections[strtab][4]
        name_i, info, _, origin, value, _ = unpack("IBBHQQ", elf, start + index * stride)
        name = c_string(elf, str_start + name_i)
        if not name:
            name = f"$s{index}".encode()
        if origin in moves:
            section, base = moves[origin]
            value += base
            section += 1
        elif origin in (0, 0xFFF1):
            section = 0 if origin == 0 else -1
        else:
            raise ValueError(f"Unsupported ELF symbol section {origin} for {name!r}")
        if len(name) > 8:
            field = pack("II", 0, len(strings))
            strings.extend(name + b"\0")
        else:
            field = name.ljust(8, b"\0")
        symbol_indices[key] = len(coff_symbols)
        storage = 3 if info >> 4 == 0 else 2
        coff_symbols.append(pack("8sIhHBB", field, value, section, 0x20 if info & 15 == 2 else 0, storage, 0))
        return symbol_indices[key]

    relocs = [[] for _ in names]
    for _, kind, _, _, start, size, symtab, target, _, stride in elf_sections:
        if kind != 4 or target not in moves:
            continue
        section, base = moves[target]
        for offset in range(start, start + size, stride):
            where, info, addend = unpack("QQq", elf, offset)
            where += base
            index = transfer_symbol(symtab, info >> 32)
            kind = info & 0xffffffff
            if kind in (2, 4):
                rel_type = 4
                data = pack("i", addend + 4)
            elif kind == 1:
                rel_type = 1
                data = pack("q", addend)
            else:
                raise ValueError(f"Unsupported AMD64 ELF relocation type {kind} in {source.name}")
            sections[section][where:where + len(data)] = data
            relocs[section].append(pack("IIH", where, index, rel_type))

    for tab, (_, kind, _, _, start, size, _, _, _, stride) in enumerate(elf_sections):
        if kind != 2:
            continue
        for index in range(1, size // stride):
            _, info, _, origin, _, _ = unpack("IBBHQQ", elf, start + index * stride)
            if info >> 4 and origin in moves:
                transfer_symbol(tab, index)

    output = bytearray(20 + 40 * len(names))
    for i, name in enumerate(names):
        content = sections[i]
        raw_offset = len(output) if content and i != 3 else 0
        if raw_offset:
            output.extend(content)
        reloc_offset = len(output) if relocs[i] else 0
        output.extend(b"".join(relocs[i]))
        struct.pack_into("<8sIIIIIIHHI", output, 20 + 40 * i, name, 0, 0, len(content),
                         raw_offset, reloc_offset, 0, len(relocs[i]), 0, flags[i])

    struct.pack_into("<HHIIIHH", output, 0, 0x8664, len(names), 0, len(output), len(coff_symbols), 0, 0)
    output.extend(b"".join(coff_symbols))
    struct.pack_into("<I", strings, 0, len(strings))
    output.extend(strings)
    destination.write_bytes(output)


def build(output):
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="autods-pe-") as temp:
        directory = Path(temp)

        def run(*args):
            subprocess.run(args, cwd=directory, check=True)

        def compile_object(source, name):
            elf = directory / f"{name}.o"
            coff = directory / f"{name}.obj"
            run("gcc", "-std=c11", "-O2", "-mabi=ms", "-mno-red-zone", "-fpie", "-fno-jump-tables",
                "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-ident",
                "-fno-asynchronous-unwind-tables", "-fstack-usage", "-Wall", "-Wextra",
                "-Werror", "-Wframe-larger-than=3700", "-c", str(source), "-o", str(elf))
            convert(elf, coff)
            usage = elf.with_suffix(".su")
            if usage.exists():
                for line in usage.read_text().splitlines():
                    if int(line.split("\t")[1]) > 3750:
                        raise RuntimeError(f"Stack frame may require Windows stack probing: {line}")
            return coff.name

        imports = collections.defaultdict(list)
        for name, dll in re.findall(r"^API\([^,]+,\s*(\w+),.*?,\s*(\w+)\)$",
                                    (HERE / "win32.h").read_text(), re.M):
            imports[dll].append(name)
        libraries = []
        for dll, names in imports.items():
            stub = directory / f"stub_{dll}.c"
            stub.write_text("\n".join(f"void {name}(void) {{}}" for name in names))
            obj = compile_object(stub, stub.stem)
            definition = directory / f"{dll}.def"
            definition.write_text(f"LIBRARY {dll}.dll\nEXPORTS\n" + "\n".join(names) + "\n")
            library = f"{dll}.a"
            run("ld", "-mi386pep", "-shared", "--entry", "0", "--out-implib", library,
                "-o", f"{dll}.dll", definition.name, obj)
            libraries.append(library)
        objects = [compile_object(HERE / name, Path(name).stem) for name in SOURCES]
        run("ld", "-mi386pep", "--entry", "ad_entry", "--subsystem", "windows:6.0", "--dynamicbase",
            "--high-entropy-va", "--nxcompat", "--no-insert-timestamp", "--stack", "2097152,16384",
            "-o", str(output), *objects, *libraries)
        run("objcopy", "--strip-all", str(output))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    build(args.output.resolve())
