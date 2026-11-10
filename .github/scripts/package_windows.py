#!/usr/bin/env python3
import os
import sys
import base64
import shutil
import hashlib
import re
import zipfile
import subprocess
import tempfile

def get_sha256(filepath):
    h = hashlib.sha256()
    with open(filepath, 'rb') as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest().upper()

def main():
    if len(sys.argv) < 4:
        print("Usage: package_windows.py <version_num> <upstream_tag> <target_tag> [repo]")
        sys.exit(1)

    version_num = sys.argv[1].strip()
    upstream_tag = sys.argv[2].strip()
    target_tag = sys.argv[3].strip()
    repo = sys.argv[4].strip() if len(sys.argv) > 4 else os.environ.get("GH_REPO", "yngvifreyr1810/Yngchii-X3D")
    upstream_match = re.fullmatch(r"fritchii-v([0-9]+(?:\.[0-9]+){1,2})", upstream_tag)
    if not upstream_match or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,4}", version_num):
        raise ValueError("Expected numeric Yngchii version and stable Fritchii tag")
    upstream_version = upstream_match[1]

    print(f"=== Starting Packaging for Yngchii-X3D v{version_num} ===")
    print(f"Upstream Tag: {upstream_tag} | Target Tag: {target_tag} | Repo: {repo}")

    staging_build = f"Yngchii-X3D-v{version_num}"
    if os.path.exists(staging_build):
        shutil.rmtree(staging_build)
    os.makedirs(staging_build, exist_ok=True)

    # A fresh staging directory avoids reusing an older archive after a failed download.
    upstream_name = f"Fritchii-v{upstream_version}-Windows-x64.zip"
    with tempfile.TemporaryDirectory(prefix="yngchii-upstream-") as scratch:
        subprocess.run(["gh", "release", "download", upstream_tag, "--repo", "Frizti/Fritchii",
                        "--pattern", upstream_name, "--dir", scratch], check=True)
        extracted = os.path.join(scratch, "extracted")
        with zipfile.ZipFile(os.path.join(scratch, upstream_name)) as zf:
            zf.extractall(extracted)
        items = os.listdir(extracted)
        root_dir = extracted
        if len(items) == 1 and os.path.isdir(os.path.join(extracted, items[0])):
            root_dir = os.path.join(extracted, items[0])
        for item in os.listdir(root_dir):
            if item.lower() == "covers":
                continue
            src = os.path.join(root_dir, item)
            dst = os.path.join(staging_build, item)
            if os.path.isdir(src):
                shutil.copytree(src, dst)
            else:
                shutil.copy2(src, dst)

    # If Yngchii-X3D.exe was not created, rename whatever exe is found
    exe_target = os.path.join(staging_build, "Yngchii-X3D.exe")
    if not os.path.exists(exe_target):
        for candidate in ["lichii.exe", "Fritchii.exe", "yuzu.exe"]:
            cp = os.path.join(staging_build, candidate)
            if os.path.exists(cp):
                shutil.move(cp, exe_target)
                break

    if not os.path.exists(exe_target):
        print("Error: Yngchii-X3D.exe not found in extracted files!")
        sys.exit(1)

    # 1.5. Require the fork's branding before preparing an artifact.
    assets_dir = os.path.join(".github", "assets")
    icon_pkl = os.path.join(assets_dir, "pochita_icons.pkl")
    rcc_ico = os.path.join(assets_dir, "pochita_rcc.ico")
    stock_rcc = base64.b64decode(subprocess.check_output(
        ["gh", "api", f"repos/Frizti/Fritchii/contents/dist/fritchii.ico?ref={upstream_tag}",
         "--jq", ".content"]
    ))
    from patch_exe_icon_inplace import patch_exe_icon
    exe_size = os.path.getsize(exe_target)
    patch_exe_icon(exe_target, icon_pkl, rcc_ico, strict=True, stock_rcc=stock_rcc)
    if os.path.getsize(exe_target) != exe_size:
        raise RuntimeError("Icon patch changed the executable size")

    # 2. Compile version.dll with dynamic version string
    assets_dir = os.path.join(".github", "assets")
    c_src_path = os.path.join(assets_dir, "version_proxy.c")
    def_path = os.path.join(assets_dir, "version_proxy.def")
    dll_out = os.path.join(staging_build, "version.dll")

    if os.path.exists(c_src_path) and os.path.exists(def_path):
        print(f"Compiling version.dll for v{version_num}...")
        # Compile the C++ proxy with llvm-mingw when available.
        compiler = shutil.which("x86_64-w64-mingw32-clang++") or shutil.which("clang++")
        compile_cmd = [
            compiler, "--target=x86_64-w64-windows-gnu", "-fuse-ld=lld",
            "-x", "c++", "-shared", "-O3", "-static", "-march=x86-64",
            f'-DYNGCHII_VERSION="{version_num}"',
            c_src_path, "-x", "none", def_path, "-o", dll_out,
            "-Wl,--delayload=gdiplus.dll,--no-insert-timestamp",
            "-luser32", "-lkernel32", "-lgdi32", "-lwinmm", "-ladvapi32",
            "-lgdiplus", "-ldelayimp", "-lshlwapi", "-lole32",
        ]
        comp_res = (
            subprocess.run(compile_cmd, capture_output=True, text=True, check=False)
            if compiler else None
        )
        if comp_res and comp_res.returncode == 0:
            print("version.dll compiled successfully!")
        else:
            error = comp_res.stderr.strip() if comp_res else "clang++ unavailable"
            fallback_dll = os.path.join(assets_dir, "version.dll")
            with open(c_src_path, "r", encoding="utf-8") as f:
                default = re.search(r'^#define YNGCHII_VERSION "([^"]+)"$', f.read(), re.MULTILINE)
            if not default or default[1] != version_num or not os.path.isfile(fallback_dll):
                raise RuntimeError(f"No version.dll for V{version_num}: {error}")
            shutil.copy2(fallback_dll, dll_out)
            print(f"Using the checked-in version.dll for V{version_num}.")
    else:
        raise FileNotFoundError("Missing Yngchii proxy source or exports")

    # CreateProcessW validates the staged PE without starting its UI.
    probe = subprocess.Popen([os.path.abspath(exe_target)], cwd=staging_build, creationflags=4)
    probe.terminate()
    probe.wait(timeout=10)

    # 3. Copy documentation & switches
    for doc in ["README.md", "HUONG_DAN_SU_DUNG.md", "HUONG DAN SU DUNG (TIENG VIET).txt", "OOTR SIMPLE HOW TO GUIDE FOR V5.txt", "nx2_switches.txt", "whats-new.md"]:
        doc_src = os.path.join(assets_dir, doc)
        if os.path.exists(doc_src):
            with open(doc_src, "r", encoding="utf-8", errors="ignore") as f:
                content = f.read()
            content = content.replace("V5.6.1.1", f"V{version_num}")
            content = content.replace("Fritchii V5.6", f"Fritchii V{upstream_version}")
            content = content.replace("Fritchii upstream V5.6", f"Fritchii upstream V{upstream_version}")
            content = content.replace("FRITCHII UPSTREAM V5.6", f"FRITCHII UPSTREAM V{upstream_version}")
            with open(os.path.join(staging_build, doc), "w", encoding="utf-8") as f:
                f.write(content)

    # 4. Create single Windows x64 zip
    zip_filename = f"Yngchii-X3D-v{version_num}-Windows-x64.zip"
    print(f"Packaging {zip_filename}...")
    if os.path.exists(zip_filename):
        os.remove(zip_filename)

    with zipfile.ZipFile(zip_filename, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        zf.mkdir(f"{staging_build}/covers/")
        for root, dirs, files in os.walk(staging_build):
            for f in files:
                fp = os.path.join(root, f)
                # Store with folder prefix Yngchii-X3D-v<version>/...
                arcname = os.path.join(staging_build, os.path.relpath(fp, staging_build))
                zf.write(fp, arcname)

    zip_size = os.path.getsize(zip_filename)
    sha256 = get_sha256(zip_filename)
    print(f"Packaged {zip_filename} ({zip_size} bytes, SHA-256: {sha256})")

    if os.environ.get("YNGCHII_PACKAGE_ONLY") == "1":
        return 0

    # 5. Upload asset to GitHub Release
    print(f"Uploading {zip_filename} to release {target_tag}...")
    up_cmd = [
        "gh", "release", "upload", target_tag, zip_filename,
        "--repo", repo,
    ]
    up_res = subprocess.run(up_cmd, capture_output=True, text=True, check=False)
    if up_res.returncode == 0:
        print(f"Successfully uploaded {zip_filename} to {target_tag}!")
    else:
        print(f"Error uploading release asset: {up_res.stderr.strip()}")
        sys.exit(1)

    print("=== Packaging & Upload Completed Successfully! ===")
    return 0

if __name__ == "__main__":
    sys.exit(main())
