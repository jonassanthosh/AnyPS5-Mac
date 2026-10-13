#!/usr/bin/env python3
import argparse
import concurrent.futures
import filecmp
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LIBRARY = Path.home() / "Library" / "Application Support" / "AnyPS5" / "Titles"
MODULE_SUFFIXES = (".prx", ".sprx")
BACKUP_SUFFIX = re.compile(r"\.(esbak|bak|orig|elf|bak_[0-9_]+)")
SHARD_BYTES = 400000
EBOOT_SHARDS = 256
LEGACY_COMPILE_FLAGS = ["-std=c++20", "-O2", "-DAPS5_TRANSLATOR_TSO=1", "-DAPS5_TRANSLATOR_VECTOR_TSO=1", "-w"]
OPTIONS = Path(__file__).resolve().parent / "options.json"


class TitleError(Exception):
    pass


def options():
    groups = json.loads(OPTIONS.read_text(encoding="utf-8"))["groups"]
    return [option for group in groups for option in group["options"]]


def read_settings(path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {"options": {}, "environment": ""}
    except json.JSONDecodeError as error:
        raise TitleError(f"{path} is not valid JSON: {error}")
    return {"options": data.get("options", {}), "environment": data.get("environment", "")}


def effective_settings(library, title_id):
    layers = [read_settings(Path(library).expanduser().parent / "settings.json"), read_settings(title_folder(library, title_id) / "settings.json")]
    chosen = {}
    run = {}
    cflags = []
    translate_flags = []
    jobs = 0
    for option in options():
        value = option["default"]
        for layer in layers:
            value = layer["options"].get(option["id"], value)
        choice = next((choice for choice in option["choices"] if choice["value"] == value), None)
        if choice is None:
            raise TitleError(f"setting {option['id']} has no choice {value!r}")
        chosen[option["id"]] = value
        run.update(choice.get("run", {}))
        build = choice.get("build", {})
        cflags += build.get("cflags", [])
        translate_flags += build.get("translate", [])
        jobs = build.get("jobs", jobs)
    for layer in layers:
        for line in layer["environment"].splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, separator, value = line.partition("=")
            if not separator or not name.strip():
                raise TitleError(f"extra environment line {line!r} is not NAME=value")
            run[name.strip()] = value.strip()
    count = os.cpu_count() or 4
    jobs = count if jobs == 0 else max(1, count // -jobs) if jobs < 0 else jobs
    return {"options": chosen, "run": run, "cflags": cflags, "translate": translate_flags, "jobs": jobs}


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


def decrypted(path):
    if is_elf(path):
        return path
    for candidate in sorted(path.parent.glob(path.name + ".*")):
        if BACKUP_SUFFIX.fullmatch(candidate.name[len(path.name):]) and is_elf(candidate):
            return candidate
    return None


def executable_folder(root):
    for folder in (root / "decrypted", root):
        if decrypted(folder / "eboot.bin") is not None:
            return folder
    if (root / "eboot.bin").exists():
        raise TitleError(f"{root / 'eboot.bin'} is encrypted and the folder has no decrypted copy of it")
    raise TitleError(f"no eboot.bin in {root} or {root / 'decrypted'}")


def replaced_by_system(path, libraries):
    return (libraries / path.name).exists() or (libraries / (path.stem + ".prx")).exists()


def image_overlay(folder, target, libraries):
    image = target / "image"
    shutil.rmtree(image, ignore_errors=True)
    image.mkdir(parents=True)
    (image / "eboot.bin").symlink_to(decrypted(folder / "eboot.bin"))
    modules = []
    for path in sorted(folder.rglob("*")):
        if path.suffix not in MODULE_SUFFIXES or not path.is_file():
            continue
        relative = path.relative_to(folder)
        if relative.parts[0] == "sce_sys":
            continue
        source = decrypted(path)
        system = replaced_by_system(path, libraries)
        if source is None:
            if system:
                continue
            raise TitleError(f"{relative} is encrypted and the folder has no decrypted copy of it")
        link = image / relative
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(source)
        if not system:
            modules.append(link)
    return image, modules


def system_libraries(repo):
    return repo / "build-arm64" / "core" / "libs" / "libs"


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
    run_checked(["ninja", "-C", str(translator), *TRANSLATOR_TARGETS], "building the translator")
    if not (repo / "build-arm64" / "build.ninja").exists():
        raise TitleError(f"{repo / 'build-arm64'} is not configured; see docs/dev/ArmTranslator.md")
    note("building the system libraries")
    run_checked(["ninja", "-C", str(repo / "build-arm64"), "libs"], "building the system libraries")
    return translator


def translate(translator, images, staging, flags):
    staging.mkdir(parents=True)
    for index, (path, prefix, shards) in enumerate(images):
        progress("translate", index, len(images))
        note(f"translating {path.name}")
        command = [str(translator / "x64a64"), str(path), "-o", str(staging / (prefix or "eboot")), "--shards", str(shards), "--image-name", path.name, "--report", *flags]
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
        if stale.suffix != ".o" and stale.name != "flags.json" and stale.name not in wanted:
            stale.unlink()
            stale.with_name(stale.name + ".o").unlink(missing_ok=True)
    for path in staging.iterdir():
        target = sources / path.name
        if target.exists() and filecmp.cmp(path, target, shallow=False):
            continue
        shutil.copy2(path, target)
        target.with_name(target.name + ".o").unlink(missing_ok=True)
    shutil.rmtree(staging)


def compile_sources(repo, sources, jobs, cflags):
    include = str(repo / "core" / "translator" / "runtime" / "include")
    stamp = sources / "flags.json"
    flags = ["-std=c++20", *cflags, "-w"]
    previous = json.loads(stamp.read_text(encoding="utf-8")) if stamp.exists() else LEGACY_COMPILE_FLAGS
    if previous != flags:
        for stale in sources.glob("*.o"):
            stale.unlink()
    stamp.write_text(json.dumps(flags), encoding="utf-8")
    pending = sorted(path for path in sources.glob("*.cpp") if not path.with_name(path.name + ".o").exists())
    total = len(pending)
    note(f"compiling {total} of {len(list(sources.glob('*.cpp')))} sources")
    progress("compile", 0, total)

    def compile_one(path):
        output = path.with_name(path.name + ".o")
        partial = path.with_name(path.name + ".o.partial")
        result = subprocess.run(["clang++", *flags, "-I", include, "-c", str(path), "-o", str(partial)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
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


TRANSLATOR_TARGETS = ["x64a64", "translator_title_main", "translator_runtime", "translator_elf"]


def plan(repo, game, library):
    root = game_root(game)
    info = parameters(root)
    target = title_folder(library, info["titleId"])
    folder, modules = image_overlay(executable_folder(root), target, system_libraries(repo))
    settings = effective_settings(library, info["titleId"])
    translator = repo / "build-translator"
    images = [(folder / "eboot.bin", "", EBOOT_SHARDS)]
    images += [(path, f"M{index}", path.stat().st_size // SHARD_BYTES + 2) for index, path in enumerate(modules, 1)]
    return {"root": root, "info": info, "folder": folder, "target": target, "settings": settings, "translator": translator, "images": images}


def stamp_of(work):
    translator = work["translator"]
    inputs = [path for path, _, _ in work["images"]] + [translator / "x64a64", translator / "libtranslator_title_main.a", translator / "libtranslator_runtime.a", translator / "libtranslator_elf.a"]
    if not all(path.exists() for path in inputs):
        return None
    return fingerprint(inputs) + ":" + hashlib.sha256(json.dumps([work["settings"]["cflags"], work["settings"]["translate"]]).encode()).hexdigest()


def status(arguments):
    repo = Path(arguments.repo).resolve()
    work = plan(repo, arguments.game, arguments.library)
    record = work["target"] / "title.json"
    prepared = (work["target"] / "title").exists() and record.exists()
    current = prepared and json.loads(record.read_text(encoding="utf-8")).get("stamp") == stamp_of(work)
    if current and (work["translator"] / "build.ninja").exists():
        pending = subprocess.run(["ninja", "-C", str(work["translator"]), "-n", *TRANSLATOR_TARGETS], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout
        current = not any(line.startswith("[") for line in pending.splitlines())
    print(json.dumps({"titleId": work["info"]["titleId"], "prepared": prepared, "current": current}))


def build(arguments):
    if arguments.own_group:
        os.setpgrp()
    repo = Path(arguments.repo).resolve()
    work = plan(repo, arguments.game, arguments.library)
    root, info, folder, target, settings, images = work["root"], work["info"], work["folder"], work["target"], work["settings"], work["images"]
    target.mkdir(parents=True, exist_ok=True)
    note(f"{info['name']} ({info['titleId']})")
    jobs = arguments.jobs or settings["jobs"]
    translator = build_tools(repo)
    stamp = stamp_of(work)
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
    translate(translator, images, staging, settings["translate"])
    (staging / "manifest.cpp").write_text(manifest(images), encoding="utf-8")
    update_sources(staging, work / "sources")
    compile_sources(repo, work / "sources", jobs, settings["cflags"])
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
    folder, _ = image_overlay(executable_folder(root), target, system_libraries(repo))
    workspace = target / "run"
    workspace.mkdir(parents=True, exist_ok=True)
    app0 = workspace / "app0"
    if app0.is_symlink() and Path(os.readlink(app0)) != root:
        app0.unlink()
    if not app0.exists():
        app0.symlink_to(root, target_is_directory=True)
    environment = {**os.environ, **effective_settings(arguments.library, info["titleId"])["run"]}
    os.chdir(workspace)
    os.execve(binary, [str(binary), str(folder), str(system_libraries(repo))], environment)


def show_settings(arguments):
    info = parameters(game_root(arguments.game))
    settings = effective_settings(arguments.library, info["titleId"])
    print(json.dumps(settings, indent=2))


def main():
    parser = argparse.ArgumentParser(description="Build and run a PS5 title natively on Apple silicon")
    parser.add_argument("--repo", default=str(REPO))
    parser.add_argument("--library", default=str(LIBRARY))
    commands = parser.add_subparsers(dest="command", required=True)
    build_command = commands.add_parser("build")
    build_command.add_argument("game")
    build_command.add_argument("--jobs", type=int)
    build_command.add_argument("--force", action="store_true")
    build_command.add_argument("--own-group", action="store_true")
    run_command = commands.add_parser("run")
    run_command.add_argument("game")
    settings_command = commands.add_parser("settings")
    settings_command.add_argument("game")
    status_command = commands.add_parser("status")
    status_command.add_argument("game")
    arguments = parser.parse_args()
    try:
        {"build": build, "run": run, "settings": show_settings, "status": status}[arguments.command](arguments)
    except TitleError as error:
        print(f"error {error}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
