#!/usr/bin/env python3
"""Fetch latest Libron and NotoSerif .cpfont bundles and emit C++ flash assets.

Runs as a PlatformIO pre-build script for environments defining BUNDLED_SD_FONTS.
Checks GitHub on each build for updates:
  - Libron: nicoverbruggen/libron latest release (Libron_CrossPoint.zip)
  - NotoSerif: crosspoint-reader/crosspoint-fonts (sd-fonts-m<M>-b<B>/fonts.json)

Caches downloaded .cpfont files in .pio/bundled_fonts_cache/ so offline builds
reuse the last verified cache if GitHub is unreachable, and skips rewriting
src/BundledSdFontsData.generated.h when nothing changed.
"""

import hashlib
import io
import json
import os
import struct
import urllib.error
import urllib.request
import zipfile
import zlib

CACHE_DIR = os.path.join(".pio", "bundled_fonts_cache")
STATE_PATH = os.path.join(CACHE_DIR, "state.json")
OUTPUT_HEADER = os.path.join("src", "BundledSdFontsData.generated.h")

LIBRON_API_URL = "https://api.github.com/repos/nicoverbruggen/libron/releases/latest"
CROSSPOINT_FONTS_MANIFEST_FMT = (
    "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/"
    "sd-fonts-m{manifest_ver}-b{cpfont_ver}/fonts.json"
)
HTTP_TIMEOUT_SEC = 15
EXPECTED_SIZES = (12, 14, 16, 18)


def should_run():
    """Return True when the active PlatformIO environment enables BUNDLED_SD_FONTS."""
    try:
        Import("env")  # type: ignore # noqa: F821
        flags = " ".join(str(x) for x in env.GetProjectOption("build_flags", []))  # type: ignore # noqa: F821
        if "BUNDLED_SD_FONTS" in flags:
            return True
        for item in env.get("CPPDEFINES", []):  # type: ignore # noqa: F821
            name = item[0] if isinstance(item, (list, tuple)) else str(item).split("=")[0]
            if name == "BUNDLED_SD_FONTS":
                return True
        return False
    except NameError:
        return True


def read_cpfont_versions():
    ns = {}
    with open(os.path.join("lib", "EpdFont", "scripts", "cpfont_version.py"), "r", encoding="utf-8") as f:
        exec(compile(f.read(), "cpfont_version.py", "exec"), ns)
    return int(ns["CPFONT_VERSION"]), int(ns["FONTS_MANIFEST_VERSION"])


def http_get_bytes(url):
    req = urllib.request.Request(url, headers={"User-Agent": "crosspoint-reader-build"})
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_SEC) as resp:
        return resp.read()


def validate_cpfont(data, expected_version):
    if len(data) < 32:
        raise ValueError(f"cpfont too small ({len(data)} bytes)")
    magic, ver, _flags, style_count, _pt = struct.unpack("<8sHHHB", data[:15])
    if magic != b"CPFONT\x00\x00":
        raise ValueError(f"bad cpfont magic: {magic!r}")
    if ver != expected_version:
        raise ValueError(f"cpfont version mismatch: got {ver}, expected {expected_version}")
    if style_count != 4:
        raise ValueError(f"expected 4 styles (regular/bold/italic/bolditalic), got {style_count}")


def load_state():
    if os.path.exists(STATE_PATH):
        with open(STATE_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_state(state):
    os.makedirs(CACHE_DIR, exist_ok=True)
    with open(STATE_PATH, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2, sort_keys=True)


def cached_file_valid(path, expected_size=None, expected_crc=None):
    if not os.path.exists(path):
        return False
    with open(path, "rb") as f:
        data = f.read()
    if expected_size is not None and len(data) != expected_size:
        return False
    if expected_crc is not None and (zlib.crc32(data) & 0xFFFFFFFF) != expected_crc:
        return False
    return True


def fetch_libron(cpfont_ver, state):
    libron_dir = os.path.join(CACHE_DIR, "Libron")
    os.makedirs(libron_dir, exist_ok=True)
    prev = state.get("libron", {})
    files_ready = all(
        cached_file_valid(os.path.join(libron_dir, f"Libron_{sz}.cpfont")) for sz in EXPECTED_SIZES
    )

    try:
        release = json.loads(http_get_bytes(LIBRON_API_URL).decode("utf-8"))
        tag = release.get("tag_name", "")
        asset = next((a for a in release.get("assets", []) if a.get("name") == "Libron_CrossPoint.zip"), None)
        if not asset:
            raise RuntimeError(f"Libron_CrossPoint.zip asset missing in release {tag}")
        asset_sig = f"{tag}:{asset.get('updated_at', '')}:{asset.get('size', 0)}"

        if files_ready and prev.get("signature") == asset_sig:
            print(f"[bundled-fonts] Libron up-to-date ({tag})")
            return tag

        print(f"[bundled-fonts] Downloading Libron ({tag}) from nicoverbruggen/libron...")
        zip_bytes = http_get_bytes(asset["browser_download_url"])
        with zipfile.ZipFile(io.BytesIO(zip_bytes)) as zf:
            extracted = {}
            for sz in EXPECTED_SIZES:
                target_name = f"Libron_{sz}.cpfont"
                match = next((n for n in zf.namelist() if os.path.basename(n) == target_name), None)
                if not match:
                    raise RuntimeError(f"Missing {target_name} inside Libron_CrossPoint.zip")
                raw = zf.read(match)
                validate_cpfont(raw, cpfont_ver)
                extracted[target_name] = raw

        for name, raw in extracted.items():
            with open(os.path.join(libron_dir, name), "wb") as f:
                f.write(raw)

        state["libron"] = {"tag": tag, "signature": asset_sig}
        print(f"[bundled-fonts] Updated Libron to {tag}")
        return tag
    except (urllib.error.URLError, TimeoutError, OSError, RuntimeError, ValueError, zipfile.BadZipFile) as exc:
        if files_ready and prev.get("tag"):
            print(f"[bundled-fonts] Warning: Libron GitHub check failed ({exc}); using cached {prev['tag']}")
            return prev["tag"]
        raise RuntimeError(f"Failed to fetch Libron fonts and no cache available: {exc}") from exc


def fetch_notoserif(cpfont_ver, manifest_ver, state):
    noto_dir = os.path.join(CACHE_DIR, "NotoSerif")
    os.makedirs(noto_dir, exist_ok=True)
    prev = state.get("notoserif", {})
    tag = f"sd-fonts-m{manifest_ver}-b{cpfont_ver}"
    files_ready = all(
        cached_file_valid(os.path.join(noto_dir, f"NotoSerif_{sz}.cpfont")) for sz in EXPECTED_SIZES
    )

    try:
        manifest_url = CROSSPOINT_FONTS_MANIFEST_FMT.format(manifest_ver=manifest_ver, cpfont_ver=cpfont_ver)
        manifest = json.loads(http_get_bytes(manifest_url).decode("utf-8"))
        base_url = manifest["baseUrl"]
        fam = next((f for f in manifest.get("families", []) if f.get("name") == "NotoSerif"), None)
        if not fam:
            raise RuntimeError(f"NotoSerif not found in crosspoint-fonts manifest ({tag})")

        file_map = {entry["name"]: entry for entry in fam.get("files", [])}
        updated_any = False
        for sz in EXPECTED_SIZES:
            name = f"NotoSerif_{sz}.cpfont"
            entry = file_map.get(name)
            if not entry:
                raise RuntimeError(f"Missing {name} in NotoSerif manifest")
            exp_size = int(entry["size"])
            exp_crc = int(entry["crc32"]) & 0xFFFFFFFF
            dest_path = os.path.join(noto_dir, name)
            if cached_file_valid(dest_path, exp_size, exp_crc):
                continue

            print(f"[bundled-fonts] Downloading {name} from crosspoint-fonts ({tag})...")
            raw = http_get_bytes(base_url + name)
            if len(raw) != exp_size or (zlib.crc32(raw) & 0xFFFFFFFF) != exp_crc:
                raise RuntimeError(f"CRC/size mismatch downloading {name}")
            validate_cpfont(raw, cpfont_ver)
            with open(dest_path, "wb") as f:
                f.write(raw)
            updated_any = True

        state["notoserif"] = {"tag": tag}
        if not updated_any:
            print(f"[bundled-fonts] NotoSerif up-to-date ({tag})")
        return tag
    except (urllib.error.URLError, TimeoutError, OSError, RuntimeError, ValueError, KeyError) as exc:
        if files_ready and prev.get("tag"):
            print(f"[bundled-fonts] Warning: NotoSerif GitHub check failed ({exc}); using cached {prev['tag']}")
            return prev["tag"]
        raise RuntimeError(f"Failed to fetch NotoSerif fonts and no cache available: {exc}") from exc


def format_c_array(name, data):
    lines = [f"alignas(4) static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append("  " + ",".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def generate_header(libron_tag, noto_tag):
    entries = []
    hasher = hashlib.sha256()
    hasher.update(f"{libron_tag}|{noto_tag}|".encode("utf-8"))

    for family in ("Libron", "NotoSerif"):
        for sz in EXPECTED_SIZES:
            filename = f"{family}_{sz}.cpfont"
            path = os.path.join(CACHE_DIR, family, filename)
            with open(path, "rb") as f:
                raw = f.read()
            crc = zlib.crc32(raw) & 0xFFFFFFFF
            sym = f"kZlib_{family}_{sz}"
            hasher.update(f"{family}/{filename}:{len(raw)}:{crc:08x};".encode("utf-8"))
            entries.append((family, filename, len(raw), crc, sym, raw))

    bundle_hash = hasher.hexdigest()[:32]
    expected_marker = f"// BUNDLE_HASH: {bundle_hash}"
    if os.path.exists(OUTPUT_HEADER):
        with open(OUTPUT_HEADER, "r", encoding="utf-8") as f:
            first_line = f.readline().rstrip("\r\n")
        if first_line == expected_marker:
            print(f"[bundled-fonts] {OUTPUT_HEADER} already matches {bundle_hash}")
            return

    print(f"[bundled-fonts] Writing compressed font bundle header ({bundle_hash})...")
    parts = [
        expected_marker,
        "// THIS FILE IS AUTOGENERATED BY scripts/fetch_bundled_fonts.py — DO NOT EDIT",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        f'static constexpr char kBundledFontsBundleHash[] = "{bundle_hash}";',
        f'static constexpr char kBundledLibronTag[] = "{libron_tag}";',
        f'static constexpr char kBundledNotoSerifTag[] = "{noto_tag}";',
        "",
    ]

    meta_rows = []
    total_zlib = 0
    total_raw = 0
    for family, filename, raw_size, crc, sym, raw in entries:
        zdata = zlib.compress(raw, level=9)
        total_raw += raw_size
        total_zlib += len(zdata)
        parts.append(format_c_array(sym, zdata))
        parts.append("")
        meta_rows.append(
            f'    {{"{family}", "{filename}", {raw_size}u, 0x{crc:08x}u, {sym}, sizeof({sym})}},'
        )

    parts.append("struct BundledCpfontAsset {")
    parts.append("  const char* family;")
    parts.append("  const char* filename;")
    parts.append("  uint32_t rawSize;")
    parts.append("  uint32_t crc32;")
    parts.append("  const uint8_t* zlibData;")
    parts.append("  uint32_t zlibSize;")
    parts.append("};")
    parts.append("")
    parts.append("static constexpr BundledCpfontAsset kBundledCpfontAssets[] = {")
    parts.extend(meta_rows)
    parts.append("};")
    parts.append("")

    with open(OUTPUT_HEADER, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))
    print(f"[bundled-fonts] Embedded {len(entries)} cpfonts: {total_raw} bytes raw -> {total_zlib} bytes zlib")


def main():
    if not should_run():
        return
    cpfont_ver, manifest_ver = read_cpfont_versions()
    state = load_state()
    libron_tag = fetch_libron(cpfont_ver, state)
    noto_tag = fetch_notoserif(cpfont_ver, manifest_ver, state)
    save_state(state)
    generate_header(libron_tag, noto_tag)


main()
