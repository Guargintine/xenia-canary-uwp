"""Apply the reviewed RSA bounds backport without modifying the submodule."""

import argparse
import hashlib
from pathlib import Path
import re


PINNED_INPUTS = {
    "wolfcrypt/src/rsa.c":
        "4315b21751b84a5e4733905b2d02e71f636ed5dce3e66598ac59f783f92fef72",
    "wolfssl/wolfcrypt/sp_int.h":
        "e77be3c0bb82c6d1678354a592228e2c92bde3fda7378d4c4e2bc31e49df26c4",
}


def apply_patch(source, patch):
    """Apply exact unified-diff contexts; offsets and fuzz are not accepted."""
    lines = patch.splitlines(keepends=True)
    outputs = {}
    index = 0
    while index < len(lines):
        if not lines[index].startswith("--- a/"):
            raise ValueError("Expected a source-file header in wolfSSL patch")
        name = lines[index][6:].rstrip("\n")
        if name not in PINNED_INPUTS or name in outputs:
            raise ValueError(f"Unexpected or duplicate patch target: {name}")
        index += 1
        if lines[index] != f"+++ b/{name}\n":
            raise ValueError(f"Unexpected destination for {name}")
        index += 1
        original = source[name].splitlines(keepends=True)
        result = []
        position = 0
        while index < len(lines) and lines[index].startswith("@@ "):
            match = re.match(r"@@ -(\d+),(\d+) \+(\d+),(\d+) @@", lines[index])
            if not match:
                raise ValueError(f"Invalid hunk header for {name}")
            old_start, old_count, new_start, new_count = map(int, match.groups())
            if old_start - 1 < position:
                raise ValueError(f"Overlapping hunks for {name}")
            result.extend(original[position:old_start - 1])
            position = old_start - 1
            if len(result) != new_start - 1:
                raise ValueError(f"Unexpected output position for {name}")
            removed = added = 0
            index += 1
            while index < len(lines) and lines[index][:1] in (" ", "+", "-"):
                if lines[index].startswith("--- a/"):
                    break
                marker, content = lines[index][0], lines[index][1:]
                if marker in (" ", "-"):
                    if position >= len(original) or original[position] != content:
                        raise ValueError(f"Patch context differs at {name}:{position + 1}")
                    position += 1
                    removed += 1
                if marker in (" ", "+"):
                    result.append(content)
                    added += 1
                index += 1
            if (removed, added) != (old_count, new_count):
                raise ValueError(f"Incorrect hunk lengths for {name}")
        result.extend(original[position:])
        outputs[name] = "".join(result)
    if outputs.keys() != PINNED_INPUTS.keys():
        raise ValueError("The wolfSSL backport must patch both pinned inputs")
    return outputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--patch", type=Path, required=True)
    args = parser.parse_args()
    source_root, output_root = args.source.resolve(), args.output.resolve()
    if output_root == source_root or source_root in output_root.parents:
        raise ValueError("wolfSSL overlay must be outside the vendor submodule")
    sources = {}
    for name, expected in PINNED_INPUTS.items():
        data = (source_root / name).read_bytes().replace(b"\r\n", b"\n")
        actual = hashlib.sha256(data).hexdigest()
        if actual != expected:
            raise ValueError(
                f"wolfSSL input changed: {name}: SHA256 {actual}, expected {expected}. "
                "Review the backport against the new vendor source before building.")
        sources[name] = data.decode("utf-8")
    outputs = apply_patch(sources, args.patch.read_text(encoding="utf-8"))
    for name, content in outputs.items():
        path = output_root / name
        data = content.encode("utf-8")
        if path.exists() and path.read_bytes() == data:
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_bytes(data)
        temporary.replace(path)


if __name__ == "__main__":
    main()
