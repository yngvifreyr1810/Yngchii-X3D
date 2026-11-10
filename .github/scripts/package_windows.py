#!/usr/bin/env python3
import os
import sys
import glob
import shutil
import hashlib
import zipfile
import subprocess

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

    print(f"=== Starting Packaging for Yngchii-X3D v{version_num} ===")
    print(f"Upstream Tag: {upstream_tag} | Target Tag: {target_tag} | Repo: {repo}")

    staging_dl = "staging_upstream_dl"
    staging_build = f"Yngchii-X3D-v{version_num}"
    os.makedirs(staging_dl, exist_ok=True)
    if os.path.exists(staging_build):
        shutil.rmtree(staging_build)
    os.makedirs(staging_build, exist_ok=True)

    # 1. Download upstream Windows x64 zip
    print(f"Downloading upstream Windows release asset from Frizti/Fritchii ({upstream_tag})...")
    dl_cmd = [
        "gh", "release", "download", upstream_tag,
        "--repo", "Frizti/Fritchii",
        "--pattern", "*Windows-x64.zip",
        "--dir", staging_dl,
        "--clobber"
    ]
    res = subprocess.run(dl_cmd, capture_output=True, text=True, check=False)
    if res.returncode != 0:
        print(f"Warning: Failed to download upstream asset: {res.stderr.strip()}")
        # Check if any zip was downloaded
        zips = glob.glob(os.path.join(staging_dl, "*.zip"))
        if not zips:
            print("No upstream Windows zip found. Skipping binary packaging for now.")
            return 0
    else:
        print("Upstream asset downloaded successfully.")

    zips = glob.glob(os.path.join(staging_dl, "*.zip"))
    if not zips:
        print("No zip file in staging_dl. Exiting.")
        return 0

    upstream_zip = zips[0]
    print(f"Extracting {upstream_zip}...")
    with zipfile.ZipFile(upstream_zip, 'r') as zf:
        zf.extractall("staging_extracted")

    # Find extracted root directory
    extracted_items = os.listdir("staging_extracted")
    if len(extracted_items) == 1 and os.path.isdir(os.path.join("staging_extracted", extracted_items[0])):
        root_dir = os.path.join("staging_extracted", extracted_items[0])
    else:
        root_dir = "staging_extracted"

    # Copy extracted files to staging_build
    for item in os.listdir(root_dir):
        s = os.path.join(root_dir, item)
        d = os.path.join(staging_build, item)
        if os.path.isdir(s):
            shutil.copytree(s, d)
        else:
            if item in ["lichii.exe", "Fritchii.exe"]:
                shutil.copy2(s, os.path.join(staging_build, "Yngchii-X3D.exe"))
            else:
                shutil.copy2(s, d)

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

    # 1.5. Patch Pochita icon in-place into Yngchii-X3D.exe
    assets_dir = os.path.join(".github", "assets")
    icon_pkl = os.path.join(assets_dir, "pochita_icons.pkl")
    if os.path.exists(icon_pkl):
        try:
            sys.path.insert(0, os.path.join(".github", "scripts"))
            from patch_exe_icon_inplace import patch_exe_icon
            patch_exe_icon(exe_target, icon_pkl)
        except Exception as e:
            print(f"Warning: Failed to patch Pochita icon: {e}")

    # 2. Compile version.dll with dynamic version string
    assets_dir = os.path.join(".github", "assets")
    c_src_path = os.path.join(assets_dir, "version_proxy.c")
    def_path = os.path.join(assets_dir, "version_proxy.def")
    dll_out = os.path.join(staging_build, "version.dll")

    if os.path.exists(c_src_path) and os.path.exists(def_path):
        print(f"Compiling version.dll for v{version_num}...")
        with open(c_src_path, "r", encoding="utf-8", errors="ignore") as f:
            c_code = f.read()

        c_code = c_code.replace("v5.6", f"v{version_num}")
        c_code = c_code.replace("5.6", f"{version_num}")
        temp_c = "temp_version_proxy.c"
        with open(temp_c, "w", encoding="utf-8") as f:
            f.write(c_code)

        # Try clang first, then gcc
        compiler = shutil.which("clang") or shutil.which("gcc") or "clang"
        compile_cmd = [
            compiler, "-shared", "-O3", "-s",
            "-o", dll_out,
            temp_c, def_path,
            "-lntdll", "-luser32", "-lgdi32", "-lkernel32", "-lcomctl32", "-lwinmm"
        ]
        comp_res = subprocess.run(compile_cmd, capture_output=True, text=True, check=False)
        if comp_res.returncode == 0:
            print("version.dll compiled successfully!")
        else:
            print(f"Warning: Compilation failed ({comp_res.stderr.strip()}). Checking fallback.")
            fallback_dll = os.path.join(assets_dir, "version.dll")
            if os.path.exists(fallback_dll):
                shutil.copy2(fallback_dll, dll_out)
                print("Copied fallback version.dll.")
    else:
        fallback_dll = os.path.join(assets_dir, "version.dll")
        if os.path.exists(fallback_dll):
            shutil.copy2(fallback_dll, dll_out)
            print("Copied fallback version.dll.")

    # 3. Copy documentation & switches
    for doc in ["README.md", "HUONG_DAN_SU_DUNG.md", "HUONG DAN SU DUNG (TIENG VIET).txt", "OOTR SIMPLE HOW TO GUIDE FOR V5.txt", "nx2_switches.txt"]:
        doc_src = os.path.join(assets_dir, doc)
        if os.path.exists(doc_src):
            with open(doc_src, "r", encoding="utf-8", errors="ignore") as f:
                content = f.read()
            content = content.replace("v5.6", f"v{version_num}").replace("5.6", f"{version_num}")
            with open(os.path.join(staging_build, doc), "w", encoding="utf-8") as f:
                f.write(content)

    # 4. Create single Windows x64 zip
    zip_filename = f"Yngchii-X3D-v{version_num}-Windows-x64.zip"
    print(f"Packaging {zip_filename}...")
    if os.path.exists(zip_filename):
        os.remove(zip_filename)

    with zipfile.ZipFile(zip_filename, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for root, dirs, files in os.walk(staging_build):
            for f in files:
                fp = os.path.join(root, f)
                # Store with folder prefix Yngchii-X3D-v<version>/...
                arcname = os.path.join(staging_build, os.path.relpath(fp, staging_build))
                zf.write(fp, arcname)

    zip_size = os.path.getsize(zip_filename)
    sha256 = get_sha256(zip_filename)
    print(f"Packaged {zip_filename} ({zip_size} bytes, SHA-256: {sha256})")

    # 5. Upload asset to GitHub Release
    print(f"Uploading {zip_filename} to release {target_tag}...")
    up_cmd = [
        "gh", "release", "upload", target_tag, zip_filename,
        "--repo", repo,
        "--clobber"
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
