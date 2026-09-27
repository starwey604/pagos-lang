"""Check observable Pagos global placement in an ELF object or linked image."""

import json
import subprocess


def verify_storage(path, expected, sizes=None):
    metadata = json.loads(subprocess.check_output(
        ["llvm-readobj", "--elf-output-style=JSON", "--sections", "--symbols", str(path)],
        text=True, timeout=20))[0]
    sections = {item["Section"]["Name"]["Name"]: item["Section"]
                for item in metadata["Sections"]}
    symbols = {item["Symbol"]["Name"]["Name"]: item["Symbol"]
               for item in metadata["Symbols"]}
    for name, (section_name, alignment, writable, nobits) in expected.items():
        symbol = symbols[f"pagos.global.{name}"]
        section = sections[section_name]
        assert symbol["Section"]["Name"] == section_name, (path, symbol)
        assert symbol["Binding"]["Name"] == "Local", (path, symbol)
        assert symbol["Type"]["Name"] == "Object", (path, symbol)
        offset = symbol["Value"] - section["Address"]
        assert symbol["Size"] > 0 and 0 <= offset <= section["Size"] - symbol["Size"], (path, symbol)
        if sizes and name in sizes:
            assert symbol["Size"] == sizes[name], (path, symbol)
        assert symbol["Value"] % alignment == 0, (path, symbol)
        assert section["AddressAlignment"] >= alignment, (path, section)
        assert section["Address"] % alignment == 0, (path, section)
        assert section["Type"]["Name"] == ("SHT_NOBITS" if nobits else "SHT_PROGBITS"), (path, section)
        flags = {flag["Name"] for flag in section["Flags"]["Flags"]}
        assert "SHF_ALLOC" in flags and "SHF_EXECINSTR" not in flags, (path, section)
        assert ("SHF_WRITE" in flags) == writable, (path, section)
