#!/usr/bin/env python3
import argparse
import concurrent.futures
import filecmp
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LIBRARY = Path.home() / "Library" / "Application Support" / "AnyPS5" / "Titles"
MODULE_SUFFIXES = (".prx", ".sprx")
SHARD_BYTES = 400000
EBOOT_SHARDS = 256
COMPILE_FLAGS = ["-std=c++20", "-O2", "-DAPS5_TRANSLATOR_TSO=1", "-DAPS5_TRANSLATOR_VECTOR_TSO=1", "-w"]
RUN_ENVIRONMENT = {"APS5_ALLOW_MSAA_GUEST_ACCESS": "1"}


class TitleError(Exception):
    pass


def progress(phase, done, total):
    print(f"progress {phase} {done} {total}", flush=True)


def note(text):
    print(text, flush=True)


def is_elf(path):
    try:
        with open(path, "rb") as file:
            return file.read(4) == b"\x7fELF"
    except OSError:
        return False


def game_root(path):
    path = Path(path).expanduser().resolve()
    for candidate in (path, path.parent):
        if (candidate / "sce_sys" / "param.json").is_file():
            return candidate
    raise TitleError(f"{path} is not a PS5 game folder: sce_sys/param.json is missing")


def parameters(root):
    data = json.loads((root / "sce_sys" / "param.json").read_text(encoding="utf-8"))
    localized = data.get("localizedParameters", {})
    language = localized.get("defaultLanguage", "en-US")
    names = localized.get(language) or localized.get("en-US") or {}
    title_id = data.get("titleId", "")
    if not title_id:
        raise TitleError("param.json has no titleId")
    return {"titleId": title_id, "name": names.get("titleName", title_id), "version": data.get("contentVersion", "")}


def executable_folder(root):
    for folder in (root / "decrypted", root):
        eboot = folder / "eboot.bin"
        if is_elf(eboot):
            return folder
        if is_elf(eboot.with_name("eboot.bin.esbak")):
            raise TitleError(f"{eboot} is signed; restore the decrypted eboot.bin from eboot.bin.esbak")
    raise TitleError(f"no decrypted eboot.bin in {root} or {root / 'decrypted'}")


def system_libraries(repo):
    return repo / "build-arm64" / "core" / "libs" / "libs"


def guest_modules(folder, libraries):
    modules = []
    for path in sorted(folder.rglob("*")):
        if path.suffix not in MODULE_SUFFIXES or not path.is_file():
            continue
        relative = path.relative_to(folder)
        if relative.parts[0] == "sce_sys" or (libraries / path.name).exists():
            continue
        if not is_elf(path):
            raise TitleError(f"{relative} is not decrypted")
        modules.append(path)
    return modules


def run_checked(command, what, cwd=None):
    result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        raise TitleError(f"{what} failed:\n{result.stdout[-4000:]}")
    return result.stdout


def build_tools(repo):
    translator = repo / "build-translator"
    if not (translator / "build.ninja").exists():
        note("configuring the translator")
        run_checked(["cmake", "-S", str(repo / "core" / "translator"), "-B", str(translator), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"], "configuring the translator")
    note("building the translator")
    run_checked(["ninja", "-C", str(translator), "x64a64", "translator_title_main", "translator_runtime", "translator_elf"], "building the translator")
    if not (repo / "build-arm64" / "build.ninja").exists():
        raise TitleError(f"{repo / 'build-arm64'} is not configured; see docs/dev/ArmTranslator.md")
    note("building the system libraries")
    run_checked(["ninja", "-C", str(repo / "build-arm64"), "libs"], "building the system libraries")
    return translator


def translate(translator, images, staging):
    staging.mkdir(parents=True)
    for index, (path, prefix, shards) in enumerate(images):
        progress("translate", index, len(images))
        note(f"translating {path.name}")
        command = [str(translator / "x64a64"), str(path), "-o", str(staging / (prefix or "eboot")), "--shards", str(shards), "--image-name", path.name, "--report"]
        if prefix:
            command += ["--symbol-prefix", prefix]
        report = run_checked(command, f"translating {path.name}")
        (staging / f"report-{prefix or 'eboot'}.txt").write_text(report, encoding="utf-8")
    progress("translate", len(images), len(images))


def manifest(images):
    lines = ['#include "translator/Runtime.hpp"']
    names = []
    for _, prefix, _ in images:
        name = "aps5TranslatedImage" + (f"_{prefix}" if prefix else "")
        lines.append(f'extern "C" const translator::TranslatedImage {name};')
        names.append("&" + name)
    lines.append(f'extern "C" const translator::TranslatedImage* const aps5TitleImages[] = {{{", ".join(names)}}};')
    lines.append(f'extern "C" const std::size_t aps5TitleImageCount = {len(names)};')
    return "\n".join(lines) + "\n"


def update_sources(staging, sources):
    sources.mkdir(parents=True, exist_ok=True)
    wanted = {path.name for path in staging.iterdir()}
    for stale in sources.iterdir():
        if stale.suffix != ".o" and stale.name not in wanted:
            stale.unlink()
            stale.with_name(stale.name + ".o").unlink(missing_ok=True)
    for path in staging.iterdir():
        target = sources / path.name
        if target.exists() and filecmp.cmp(path, target, shallow=False):
            continue
        shutil.copy2(path, target)
        target.with_name(target.name + ".o").unlink(missing_ok=True)
    shutil.rmtree(staging)


def compile_sources(repo, sources, jobs):
    include = str(repo / "core" / "translator" / "runtime" / "include")
    pending = sorted(path for path in sources.glob("*.cpp") if not path.with_name(path.name + ".o").exists())
    total = len(pending)
    note(f"compiling {total} of {len(list(sources.glob('*.cpp')))} sources")
    progress("compile", 0, total)

    def compile_one(path):
        output = path.with_name(path.name + ".o")
        partial = path.with_name(path.name + ".o.partial")
        result = subprocess.run(["clang++", *COMPILE_FLAGS, "-I", include, "-c", str(path), "-o", str(partial)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode != 0:
            partial.unlink(missing_ok=True)
            return path.name, result.stdout
        partial.rename(output)
        return path.name, None

    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for done, (name, error) in enumerate(pool.map(compile_one, pending), 1):
            if error is not None:
                failures.append((name, error))
            progress("compile", done, total)
    if failures:
        name, error = failures[0]
        raise TitleError(f"compiling {len(failures)} sources failed; first, {name}:\n{error[-4000:]}")


def link(translator, sources, binary):
    progress("link", 0, 1)
    note("linking")
    partial = binary.with_name(binary.name + ".partial")
    objects = sorted(str(path) for path in sources.glob("*.o"))
    libraries = [str(translator / name) for name in ("libtranslator_title_main.a", "libtranslator_runtime.a", "libtranslator_elf.a")]
    run_checked(["clang++", *objects, *libraries, "-framework", "CoreFoundation", "-o", str(partial)], "linking")
    partial.rename(binary)
    progress("link", 1, 1)


def fingerprint(paths):
    digest = hashlib.sha256()
    for path in paths:
        status = path.stat()
        digest.update(f"{path}\0{status.st_size}\0{status.st_mtime_ns}\0".encode())
    return digest.hexdigest()


def title_folder(library, title_id):
    return Path(library).expanduser() / title_id


def build(arguments):
    if arguments.own_group:
        os.setpgrp()
    repo = Path(arguments.repo).resolve()
    root = game_root(arguments.game)
    info = parameters(root)
    folder = executable_folder(root)
    target = title_folder(arguments.library, info["titleId"])
    target.mkdir(parents=True, exist_ok=True)
    note(f"{info['name']} ({info['titleId']})")
    translator = build_tools(repo)
    modules = guest_modules(folder, system_libraries(repo))
    images = [(folder / "eboot.bin", "", EBOOT_SHARDS)]
    images += [(path, f"M{index}", path.stat().st_size // SHARD_BYTES + 2) for index, path in enumerate(modules, 1)]
    inputs = [path for path, _, _ in images] + [translator / "x64a64", translator / "libtranslator_title_main.a", translator / "libtranslator_runtime.a", translator / "libtranslator_elf.a"]
    stamp = fingerprint(inputs)
    binary = target / "title"
    record = target / "title.json"
    if binary.exists() and record.exists() and json.loads(record.read_text(encoding="utf-8")).get("stamp") == stamp and not arguments.force:
        note("already up to date")
        progress("done", 1, 1)
        return
    record.unlink(missing_ok=True)
    work = target / "build"
    staging = work / "staging"
    shutil.rmtree(staging, ignore_errors=True)
    translate(translator, images, staging)
    (staging / "manifest.cpp").write_text(manifest(images), encoding="utf-8")
    update_sources(staging, work / "sources")
    compile_sources(repo, work / "sources", arguments.jobs)
    link(translator, work / "sources", binary)
    record.write_text(json.dumps({**info, "game": str(root), "executableFolder": str(folder), "repo": str(repo), "stamp": stamp}, indent=2), encoding="utf-8")
    progress("done", 1, 1)
    note("ready")


def run(arguments):
    repo = Path(arguments.repo).resolve()
    root = game_root(arguments.game)
    info = parameters(root)
    target = title_folder(arguments.library, info["titleId"])
    binary = target / "title"
    if not binary.exists() or not (target / "title.json").exists():
        raise TitleError(f"{info['name']} is not built yet; run: {Path(__file__).name} build {root}")
    folder = executable_folder(root)
    workspace = target / "run"
    workspace.mkdir(parents=True, exist_ok=True)
    app0 = workspace / "app0"
    if app0.is_symlink() and Path(os.readlink(app0)) != root:
        app0.unlink()
    if not app0.exists():
        app0.symlink_to(root, target_is_directory=True)
    environment = {**os.environ, **RUN_ENVIRONMENT}
    os.chdir(workspace)
    os.execve(binary, [str(binary), str(folder), str(system_libraries(repo))], environment)


def main():
    parser = argparse.ArgumentParser(description="Build and run a PS5 title natively on Apple silicon")
    parser.add_argument("--repo", default=str(REPO))
    parser.add_argument("--library", default=str(LIBRARY))
    commands = parser.add_subparsers(dest="command", required=True)
    build_command = commands.add_parser("build")
    build_command.add_argument("game")
    build_command.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    build_command.add_argument("--force", action="store_true")
    build_command.add_argument("--own-group", action="store_true")
    run_command = commands.add_parser("run")
    run_command.add_argument("game")
    arguments = parser.parse_args()
    try:
        {"build": build, "run": run}[arguments.command](arguments)
    except TitleError as error:
        print(f"error {error}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
