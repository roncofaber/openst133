# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Check that the native, Python and ScopeFoundry versions, the changelog and an optional release tag agree."""

from pathlib import Path
import re
import sys


def version_from(pattern, text, source):
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        raise ValueError(f"cannot find the release version in {source}")
    return match.group(1)


def main():
    if len(sys.argv) > 2 or (len(sys.argv) == 2 and not sys.argv[1].startswith("v")):
        raise ValueError("usage: check_version.py [vMAJOR.MINOR.PATCH]")
    root = Path(__file__).resolve().parents[2]
    pyproject = r'^version = "([0-9]+\.[0-9]+\.[0-9]+)"$'
    versions = {
        "native": version_from(r"^project\(openst133 VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C\)$", (root / "CMakeLists.txt").read_text(), "CMakeLists.txt"),
        "Python": version_from(pyproject, (root / "pyproject.toml").read_text(), "pyproject.toml"),
        "ScopeFoundry": version_from(pyproject, (root / "integrations/scopefoundry/pyproject.toml").read_text(), "integrations/scopefoundry/pyproject.toml"),
    }
    version = versions["native"]
    if len(set(versions.values())) != 1:
        raise ValueError("versions disagree: " + ", ".join(f"{name} {value}" for name, value in versions.items()))
    if len(sys.argv) == 2 and sys.argv[1][1:] != version:
        raise ValueError(f"tag {sys.argv[1]} does not match version {version}")
    changelog = (root / "CHANGELOG.md").read_text()
    if re.search(rf"^## \[{re.escape(version)}\] - [0-9]{{4}}-[0-9]{{2}}-[0-9]{{2}}$", changelog, re.MULTILINE) is None:
        raise ValueError(f"CHANGELOG.md has no dated release section for {version}")
    subject = f"release tag {sys.argv[1]}" if len(sys.argv) == 2 else f"version {version}"
    print(f"{subject} matches the native, Python, ScopeFoundry and changelog versions")


if __name__ == "__main__":
    main()
