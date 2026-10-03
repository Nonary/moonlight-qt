#!/usr/bin/env python3
"""Refresh only PyroWave's three embedded dequant shaders.

Run in the development container with glslangValidator and spirv-val on PATH.
The other shaders and their reflection are preserved. Descriptor bindings and
push constants are unchanged; specialization ID 0 is the only reflection change.
Full upstream slangmosh regeneration remains supported by slangmosh.json.
"""
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent
    shaders = root / "pyrowave/pyrowave/shaders"
    header = shaders / "slangmosh.hpp"
    text = header.read_text()
    bank_match = re.search(r"static const uint32_t spirv_bank\[\] =\s*\{(.*?)\n\};", text, re.S)
    reflection_match = re.search(r"static const uint8_t reflection_bank\[\] =\s*\{(.*?)\n\};", text, re.S)
    if not bank_match or not reflection_match:
        raise RuntimeError("Unrecognized slangmosh bank format")
    words = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", bank_match[1])]
    reflection = bytearray(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", reflection_match[1]))
    pattern = (r"layout\.unserialize\(reflection_bank \+ (\d+), (\d+)\);\s*"
               r"this->wavelet_dequant\[(\d+)\] = device\.request_program\(spirv_bank \+ (\d+), (\d+), &layout\);")
    entries = list(re.finditer(pattern, text))
    if len(entries) != 3 or [int(m[3]) for m in entries] != [0, 1, 2]:
        raise RuntimeError("Expected three dequant variants")
    start = int(entries[0][4])
    end = start
    for m in entries:
        if int(m[4]) != end:
            raise RuntimeError("Dequant bank is not contiguous")
        end += int(m[5]) // 4
    if end != len(words):
        raise RuntimeError("Dequant is no longer the final bank; update this generator")
    words = words[:start]
    replacements = []
    with tempfile.TemporaryDirectory(prefix="pyrowave-dequant-") as directory:
        for m in entries:
            variant = int(m[3])
            binary = Path(directory) / f"dequant-{variant}.spv"
            subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.1", "-Os", "-g0",
                            f"-DSTORAGE_MODE={variant}", f"-I{shaders}",
                            str(shaders / "wavelet_dequant.comp"), "-o", str(binary)], check=True)
            subprocess.run(["spirv-val", "--target-env", "vulkan1.1", str(binary)], check=True)
            data = binary.read_bytes()
            offset = len(words)
            words.extend(struct.unpack(f"<{len(data) // 4}I", data))
            reflection_offset, reflection_size = int(m[1]), int(m[2])
            if reflection_size != 348 or reflection[reflection_offset:reflection_offset + 8] != b"G\0R\0A\0\x07\0":
                raise RuntimeError("ResourceLayout version/size changed; regenerate with slangmosh")
            # ResourceLayout v7 ends in spec_constant_mask, bindless_set_mask.
            spec_offset = reflection_offset + reflection_size - 8
            old_mask, = struct.unpack_from("<I", reflection, spec_offset)
            if old_mask not in (0, 1):
                raise RuntimeError("Unexpected specialization constants")
            struct.pack_into("<I", reflection, spec_offset, 1)
            replacements.append((m.start(), m.end(),
                f"layout.unserialize(reflection_bank + {reflection_offset}, {reflection_size});\n"
                f"\tthis->wavelet_dequant[{variant}] = device.request_program(spirv_bank + {offset}, {len(data)}, &layout);"))
    def rows(values, count, fmt):
        return "\n" + "\n".join("\t" + ", ".join(fmt.format(x) for x in values[i:i + count]) + ","
                                  for i in range(0, len(values), count))
    replacements.extend([
        (bank_match.start(1), bank_match.end(1), rows(words, 8, "0x{:08x}u")),
        (reflection_match.start(1), reflection_match.end(1), rows(reflection, 32, "0x{:02x}")),
    ])
    for begin, end, replacement in sorted(replacements, reverse=True):
        text = text[:begin] + replacement + text[end:]
    header.write_text(text)


if __name__ == "__main__":
    main()
