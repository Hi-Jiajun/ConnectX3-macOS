#!/usr/bin/env python3
"""Generate matching build metadata in a private build staging directory.

The source plist owns the numeric release version; BUILD_TAG identifies a
candidate within that release. This does not identify a loaded kext or boot.
"""
import argparse
import json
from pathlib import Path
import plistlib
import re
import sys


def stage_identity(source, output_plist, output_header, build_tag):
    source, output_plist, output_header = map(Path, (source, output_plist, output_header))
    if len({path.resolve() for path in (source, output_plist, output_header)}) != 3:
        raise ValueError("source plist and the two staged outputs must be distinct")
    if not isinstance(build_tag, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+\-]{0,63}", build_tag):
        raise ValueError("build tag must be 1-64 safe ASCII label characters")
    with source.open("rb") as stream:
        info = plistlib.load(stream)
    if not isinstance(info, dict):
        raise ValueError("source plist must be a dictionary")
    version = info.get("CFBundleVersion")
    # This build workflow supports numeric release versions, not candidate
    # suffixes in CFBundleVersion. Preserve the latter separately in BUILD_TAG.
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", version):
        raise ValueError("CFBundleVersion must be an explicit numeric release version")
    info["CX3BuildTag"] = build_tag
    encoded = plistlib.dumps(info, sort_keys=False)
    header = ("/* Generated in private build staging; source plist owns release version. */\n"
              "#ifndef CX3_GENERATED_BUILD_IDENTITY_H\n#define CX3_GENERATED_BUILD_IDENTITY_H\n"
              f"#define CX3_BUILD_TAG {json.dumps(build_tag)}\n"
              f"#define CX3_BUNDLE_VERSION {json.dumps(version)}\n#endif\n")
    # Validate and serialize both inputs before touching either staged output.
    output_plist.write_bytes(encoded)
    output_header.write_text(header, encoding="ascii")
    return {"bundle_version": version, "build_tag": build_tag}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_plist")
    parser.add_argument("output_plist")
    parser.add_argument("output_header")
    parser.add_argument("build_tag")
    args = parser.parse_args()
    try:
        result = stage_identity(args.source_plist, args.output_plist,
                                args.output_header, args.build_tag)
    except (OSError, ValueError, TypeError, OverflowError, plistlib.InvalidFileException) as error:
        print(f"stage-build-identity: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
