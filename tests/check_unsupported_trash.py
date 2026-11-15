"""Run the real Linux UI/document regressions with only Trash capability false.

Uses the existing Unix Makefiles build's compile/link commands. This substitutes
one capability function, not Windows filesystem behavior; native Windows CI
separately checks the actual platform capability and adapter.
"""
import json
import pathlib
import shlex
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
build = pathlib.Path(sys.argv[1]).resolve()
fixture = build / "unsupported-trash"
fixture.mkdir(exist_ok=True)
source = fixture / "FileTrash.cpp"
source.write_text(
    '#define fileTrashSupported nativeFileTrashSupported\n'
    f'#include "{root / "src/platform/FileTrash.cpp"}"\n'
    '#undef fileTrashSupported\n'
    'bool fileTrashSupported() { return false; }\n'
)
commands = json.loads((build / "compile_commands.json").read_text())
entry = next(c for c in commands if c["file"] == str(root / "src/platform/FileTrash.cpp"))
compile_command = shlex.split(entry["command"])
obj = fixture / "FileTrash.cpp.o"
compile_command[compile_command.index("-o") + 1] = str(obj)
compile_command[compile_command.index("-c") + 1] = str(source)
subprocess.run(compile_command, cwd=entry["directory"], check=True)

link_command = shlex.split((build / "CMakeFiles/OfxRawHost.dir/link.txt").read_text())
link_command[link_command.index("CMakeFiles/OfxRawHost.dir/src/platform/FileTrash.cpp.o")] = str(obj)
binary = build / "OfxRawHostUnsupportedTrash"
link_command[link_command.index("-o") + 1] = str(binary)
subprocess.run(link_command, cwd=build, check=True)
subprocess.run([str(binary), "--selftest-photo"], cwd=root, check=True)
