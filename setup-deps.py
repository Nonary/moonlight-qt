import argparse
import glob
import json
import os
import sys
import platform
import tempfile
import urllib.request
import zipfile
import shutil

ORGANIZATION = "moonlight-stream"
PREBUILT_REPO = "moonlight-qt-deps"
TAG = "v8"

# Written last into a fully extracted deployment. Its presence, a matching
# tag/asset, and every recorded file establish that the deployment is usable.
COMPLETION_MARKER = ".moonlight-deps.json"
REQUIRED_DIRECTORIES = ("lib", "include")


def get_platform_config():
    system = platform.system()
    if system == "Darwin":
        return "mac", "macos-universal.zip"
    if system == "Linux":
        return "steamlink", "steamlink.zip"

    print(f"Error: Unsupported platform ({system})")
    sys.exit(1)


def deployment_problem(target_dir, tag, asset_name):
    """Return why target_dir cannot be reused, or None if it is complete."""
    marker_path = os.path.join(target_dir, COMPLETION_MARKER)
    try:
        with open(marker_path, "r", encoding="utf-8") as marker_file:
            marker = json.load(marker_file)
    except FileNotFoundError:
        return "no completed deployment was recorded"
    except (OSError, ValueError) as e:
        return f"completion marker is unreadable ({e})"

    if not isinstance(marker, dict):
        return "completion marker is malformed"
    if marker.get("tag") != tag or marker.get("asset") != asset_name:
        return f"deployed {marker.get('asset')}@{marker.get('tag')} does not match {asset_name}@{tag}"

    files = marker.get("files")
    if not isinstance(files, dict) or not files:
        return "completion marker lists no files"
    for name, size in files.items():
        path = os.path.join(target_dir, name)
        if not os.path.isfile(path):
            return f"{name} is missing"
        if os.path.getsize(path) != size:
            return f"{name} has an unexpected size"
    for name in REQUIRED_DIRECTORIES:
        if not os.path.isdir(os.path.join(target_dir, name)):
            return f"{name}/ is missing"
    return None


def extract_archive(archive_path, destination):
    """Extract the archive and return the regular files it contained, by size."""
    files = {}
    with zipfile.ZipFile(archive_path, "r") as zip_ref:
        for member in zip_ref.infolist():
            zip_ref.extract(member, destination)
            if not member.is_dir():
                files[member.filename.rstrip("/")] = member.file_size
    return files


def download_and_extract(force=False, root=None, tag=None):
    tag = TAG if tag is None else tag
    root = os.getcwd() if root is None else root
    subfolder, asset_name = get_platform_config()

    libs_dir = os.path.join(root, "libs")
    target_dir = os.path.join(libs_dir, subfolder)
    url = f"https://github.com/{ORGANIZATION}/{PREBUILT_REPO}/releases/download/{tag}/{asset_name}"

    if not force:
        problem = deployment_problem(target_dir, tag, asset_name)
        if problem is None:
            print(f"Dependencies {asset_name}@{tag} already deployed. Use --force to re-download.")
            return
        if os.path.exists(target_dir):
            print(f"Redeploying dependencies: {problem}")

    os.makedirs(libs_dir, exist_ok=True)

    # Remove staging trees left behind by interrupted runs.
    for stale in glob.glob(os.path.join(libs_dir, f".{subfolder}.staging-*")):
        shutil.rmtree(stale, ignore_errors=True)

    # Build the complete deployment beside the target and publish it with a
    # rename, so an interruption never leaves a partial tree at target_dir.
    staging_dir = tempfile.mkdtemp(prefix=f".{subfolder}.staging-", dir=libs_dir)
    try:
        archive_path = os.path.join(staging_dir, asset_name)

        print(f"Downloading {asset_name}...")
        try:
            urllib.request.urlretrieve(url, archive_path)
        except Exception as e:
            print(f"Download failed: {e}")
            sys.exit(1)

        print(f"Extracting {asset_name}...")
        files = extract_archive(archive_path, staging_dir)
        os.remove(archive_path)

        missing = [name for name in REQUIRED_DIRECTORIES
                   if not os.path.isdir(os.path.join(staging_dir, name))]
        if not files or missing:
            print(f"Error: {asset_name}@{tag} is missing required contents: {', '.join(missing) or 'files'}")
            sys.exit(1)

        with open(os.path.join(staging_dir, COMPLETION_MARKER), "w", encoding="utf-8") as marker_file:
            json.dump({"tag": tag, "asset": asset_name, "url": url, "files": files},
                      marker_file, indent=2, sort_keys=True)

        problem = deployment_problem(staging_dir, tag, asset_name)
        if problem is not None:
            print(f"Error: extracted dependencies are incomplete: {problem}")
            sys.exit(1)

        if os.path.exists(target_dir):
            print("Cleaning target directory...")
            shutil.rmtree(target_dir)
        os.replace(staging_dir, target_dir)
    finally:
        shutil.rmtree(staging_dir, ignore_errors=True)

    print(f"Dependencies successfully deployed")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Download prebuilt Moonlight dependencies.")
    parser.add_argument("--force", action="store_true",
                        help="re-download even if a complete matching deployment exists")
    args = parser.parse_args(argv)
    download_and_extract(force=args.force)


if __name__ == "__main__":
    main()
