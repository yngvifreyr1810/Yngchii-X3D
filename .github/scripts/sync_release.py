#!/usr/bin/env python3
import os
import sys
import datetime
import subprocess

def main():
    if len(sys.argv) < 3:
        print("Usage: sync_release.py <version_num> <latest_upstream_tag>")
        sys.exit(1)

    version_num = sys.argv[1].strip()
    upstream_tag = sys.argv[2].strip()
    today = datetime.date.today().isoformat()

    # 1. Read previous version
    prev_version = "5.1"
    version_file = os.path.join("dist", "frizti_version.txt")
    if os.path.exists(version_file):
        try:
            with open(version_file, "r", encoding="utf-8") as f:
                val = f.read().strip()
                if val:
                    prev_version = val
        except Exception as e:
            print(f"Warning reading {version_file}: {e}")

    # 2. Get git log between tags
    commits_list = []
    try:
        cmd = ["git", "log", "--oneline", f"fritchii-v{prev_version}..{upstream_tag}"]
        res = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if res.returncode == 0 and res.stdout.strip():
            for line in res.stdout.strip().splitlines():
                commits_list.append(f"- {line.strip()}")
    except Exception as e:
        print(f"Warning running git log: {e}")

    if not commits_list:
        commits_list = [f"- Upstream synchronizations from {upstream_tag}"]

    commits_summary = "\n".join(commits_list)

    # 3. Update dist/frizti_version.txt
    os.makedirs("dist", exist_ok=True)
    with open(version_file, "w", encoding="utf-8") as f:
        f.write(f"{version_num}\n")
    print(f"Updated {version_file} to {version_num}")

    # 4. Prepend to CHANGELOG.md
    changelog_entry = (
        f"## [{version_num}.0] - {today}\n\n"
        f"### Upstream Fritchii v{version_num} Synchronizations\n"
        f"{commits_summary}\n\n"
        f"### Yngchii-X3D Optimizations Maintained\n"
        f"- Dedicated AMD Ryzen 3D V-Cache thread core pinning (GPUFront Core 1, GPUBack Core 2).\n"
        f"- Hardware-gated AVX-512 dirty page scan with clean AVX2 fallback.\n"
        f"- In-place auto-updater targeting Yngchii-X3D releases with pre-release safety.\n"
        f"- Selective NVIDIA Overlay & Game Filter bypass.\n\n"
        f"---\n"
    )

    changelog_path = "CHANGELOG.md"
    content = ""
    if os.path.exists(changelog_path):
        with open(changelog_path, "r", encoding="utf-8") as f:
            content = f.read()

    header_pos = content.find("---")
    if header_pos != -1:
        new_content = content[:header_pos + 4] + "\n" + changelog_entry + "\n" + content[header_pos + 4:]
    else:
        new_content = changelog_entry + "\n" + content

    with open(changelog_path, "w", encoding="utf-8") as f:
        f.write(new_content)
    print(f"Prepended v{version_num} entry to {changelog_path}")

    # 5. Generate release_notes.md
    release_notes = (
        f"## Yngchii-X3D v{version_num} Pre-Release\n\n"
        f"This pre-release automatically synchronizes all upstream bug fixes from **Fritchii v{version_num}**:\n"
        f"- Upstream release tag: `{upstream_tag}`\n"
        f"- Preserves full AMD Ryzen 7800X3D / 9800X3D CCD thread pinning and zero-sleep futex synchronization.\n"
        f"- Incorporates latest upstream fixes and improvements.\n\n"
        f"### Upstream Commits Synchronized\n"
        f"{commits_summary}\n\n"
        f"> **Notice for Users**: This is an automated sync pre-release. Verified release binaries will be uploaded shortly.\n"
    )

    with open("release_notes.md", "w", encoding="utf-8") as f:
        f.write(release_notes)
    print("Generated release_notes.md successfully.")

if __name__ == "__main__":
    main()
