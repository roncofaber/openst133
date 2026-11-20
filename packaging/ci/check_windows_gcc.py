# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Compile a trivial C file with the MSYS2 UCRT64 gcc from the wheel build environment and print any failure."""

import os
import pathlib
import subprocess
import sys
import tempfile

GCC = "C:/msys64/ucrt64/bin/gcc.exe"

directory = pathlib.Path(tempfile.mkdtemp())
source = directory / "check.c"
source.write_text("int main(void) { return 0; }\n")
print("PATH starts with:", os.environ.get("PATH", "")[:240])
result = subprocess.run([GCC, "-v", "-c", str(source), "-o", str(directory / "check.o")], capture_output=True, text=True)
print(result.stdout)
print(result.stderr)
print("gcc exit status:", result.returncode)
sys.exit(result.returncode)
