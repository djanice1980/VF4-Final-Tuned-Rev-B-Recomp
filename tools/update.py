#!/usr/bin/env python3
"""Updates a release folder from the project's GitHub releases (docs/design.md,
section 24). The launcher runs it with the release's own Python; it changes
nothing outside <release>/update/. Putting the new files in place is the
launcher's work (it has to move its own exe aside).

  python -I tools/update.py check <release folder>
      the newest release for this platform, from <release>/release.json
      (version, platform, repo): "@update none", or "@update VERSION SIZE"

  python -I tools/update.py fetch <release folder> VERSION [--launcher]
      downloads that release's package for this platform, checks it against
      the SHA-256 GitHub gives, and unpacks it into <release>/update/new, then
      writes <release>/update/ready.json. Progress as the builder's
      ("@stage N OF TEXT", "@pct P"), then "@ready VERSION". With --launcher,
      standard input closing cancels it (exit code 3).

Errors are "error: ..." lines (exit code 1). Pre-releases count as releases.
VF4FT_UPDATE_API sets another API address than https://api.github.com (tests:
a local copy of the releases list and its packages).
"""
import hashlib
import json
import os
import re
import shutil
import sys
import time
import urllib.error
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vf4build import proc  # noqa: E402

AGENT = "vf4ft-launcher"


class UpdateError(Exception):
    pass


def version_key(v):
    """'0.6.0' or 'v0.6.0' -> a sortable key; a '-dev' or other suffix sorts
    before the release itself. None when it is not a version."""
    m = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?", (v or "").strip())
    if not m:
        return None
    return (int(m[1]), int(m[2]), int(m[3]), 0 if m[4] else 1, m[4] or "")


def read_release(folder):
    path = os.path.join(folder, "release.json")
    try:
        with open(path, encoding="utf-8") as f:
            info = json.load(f)
    except (OSError, ValueError) as e:
        raise UpdateError("%s: %s (only a release folder updates itself)" % (path, e))
    for k in ("version", "platform", "repo"):
        if not isinstance(info.get(k), str) or not info[k]:
            raise UpdateError("%s has no %s" % (path, k))
    if version_key(info["version"]) is None:
        raise UpdateError("%s: %r is not a version" % (path, info["version"]))
    return info


def _get(url, timeout=30):
    return urllib.request.urlopen(urllib.request.Request(url, headers={
        "User-Agent": AGENT, "Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"}),
        timeout=timeout)


def releases(repo):
    api = os.environ.get("VF4FT_UPDATE_API", "https://api.github.com").rstrip("/")
    url = "%s/repos/%s/releases?per_page=30" % (api, repo)
    try:
        with _get(url) as r:
            data = json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise UpdateError("no releases to see at github.com/%s (a private repository's are not shown "
                              "without signing in)" % repo)
        if e.code in (403, 429):
            raise UpdateError("GitHub asks to wait before checking again (HTTP %d)" % e.code)
        raise UpdateError("GitHub answered HTTP %d" % e.code)
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise UpdateError("cannot reach GitHub: %s" % getattr(e, "reason", e))
    if not isinstance(data, list):
        raise UpdateError("GitHub's answer is not a list of releases")
    return data


def package_of(rel, platform):
    """the release's package for this platform: vf4ft-<version>-<platform>.zip"""
    for a in rel.get("assets") or []:
        name = a.get("name") or ""
        if name.startswith("vf4ft-") and name.endswith("-%s.zip" % platform) and a.get("browser_download_url"):
            return a
    return None


def newest(info):
    """(version, release, asset) of the newest release newer than this one
    that has a package for this platform, or None"""
    here = version_key(info["version"])
    best = None
    for rel in releases(info["repo"]):
        if rel.get("draft"):
            continue
        k = version_key(rel.get("tag_name"))
        a = package_of(rel, info["platform"])
        if k and a and k > here and (best is None or k > best[0]):
            best = (k, rel, a)
    if not best:
        return None
    return best[1]["tag_name"].lstrip("v"), best[1], best[2]


def clean(folder, info):
    """what an update left in <release>/update: the old files the launcher
    moved aside (update/old-TIME, kept until the old launcher has ended),
    unfinished downloads, and a downloaded update that is not newer"""
    upd = os.path.join(folder, "update")
    if not os.path.isdir(upd):
        return
    ready = None
    try:
        with open(os.path.join(upd, "ready.json"), encoding="utf-8") as f:
            ready = json.load(f).get("version")
    except (OSError, ValueError):
        pass
    stale = not (ready and version_key(ready) and version_key(ready) > version_key(info["version"]))
    # update/new still there means no update went in since: an old-* with files
    # in it is then what a failed one could not move back, the only copy
    unfinished = os.path.isdir(os.path.join(upd, "new"))
    for name in os.listdir(upd):
        p = os.path.join(upd, name)
        if name.startswith("old-") and unfinished and os.path.isdir(p) and os.listdir(p):
            print("kept update/%s: an update did not finish, and these are old files (launcher.log)" % name,
                  flush=True)
            continue
        if name.startswith("old-") or name.endswith(".part") or (stale and name in ("new", "ready.json")):
            # rmtree does not follow links (a junction or symlink is removed, not what it points at)
            if os.path.isdir(p) and not os.path.islink(p) and not getattr(os.path, "isjunction", lambda _: False)(p):
                shutil.rmtree(p, ignore_errors=True)
            else:
                try:
                    os.remove(p)
                except OSError:
                    pass
            print("removed update/%s%s" % (name, "" if not os.path.exists(p) else " (not all: in use?)"), flush=True)


def check(folder):
    info = read_release(folder)
    clean(folder, info)
    print("this is %s for %s; asking github.com/%s" % (info["version"], info["platform"], info["repo"]), flush=True)
    n = newest(info)
    if not n:
        print("@update none", flush=True)
        return 0
    v, rel, a = n
    print("%s is out%s (%d MB)" % (v, " (a pre-release)" if rel.get("prerelease") else "", a.get("size", 0) // 1000000),
          flush=True)
    print("@update %s %d" % (v, a.get("size", 0)), flush=True)
    return 0


class Progress:
    def __init__(self):
        self.last = None

    def stage(self, n, of, text):
        print("@stage %d %d %s" % (n, of, text), flush=True)
        self.last = None

    def pct(self, v):
        v = int(v)
        if v != self.last:
            print("@pct %d" % v, flush=True)
            self.last = v


def download(a, dest, prog):
    url, size = a["browser_download_url"], int(a.get("size") or 0)
    digest = (a.get("digest") or "").lower()
    want = digest[7:] if digest.startswith("sha256:") else None
    print("downloading %s (%d MB)" % (a["name"], size // 1000000), flush=True)
    h, done = hashlib.sha256(), 0
    with _get(url, timeout=60) as r, open(dest, "wb") as f:
        while True:
            b = r.read(1 << 18)
            if not b:
                break
            f.write(b)
            h.update(b)
            done += len(b)
            if size:
                prog.pct(min(100, 100 * done / size))
    if size and done != size:
        raise UpdateError("%s: %d bytes, not the %d GitHub lists (download cut short?)" % (a["name"], done, size))
    if want:
        if h.hexdigest() != want:
            raise UpdateError("%s: SHA-256 %s, not GitHub's %s (download damaged)" % (a["name"], h.hexdigest(), want))
        print("SHA-256 matches GitHub's", flush=True)
    else:
        print("GitHub gives no SHA-256 for it: checked by its size only", flush=True)


def unpack(zpath, dest, prog):
    """the package's one top folder's contents into dest; no path may leave it"""
    with zipfile.ZipFile(zpath) as z:
        infos = z.infolist()
        tops = {i.filename.replace("\\", "/").split("/", 1)[0] for i in infos}
        if len(tops) != 1:
            raise UpdateError("the package has %d top folders, not one" % len(tops))
        top = tops.pop() + "/"
        real = os.path.realpath(dest)
        for n, i in enumerate(infos):
            name = i.filename.replace("\\", "/")
            rel = name[len(top):]
            if not rel:
                continue
            parts = rel.split("/")
            if name.startswith("/") or ":" in parts[0] or ".." in parts:
                raise UpdateError("the package has a path outside its folder: %s" % name)
            out = os.path.realpath(os.path.join(dest, *[p for p in parts if p]))
            if not out.startswith(real + os.sep):
                raise UpdateError("the package has a path outside its folder: %s" % name)
            if i.is_dir():
                os.makedirs(out, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with z.open(i) as src, open(out, "wb") as f:
                    shutil.copyfileobj(src, f, 1 << 20)
                mode = (i.external_attr >> 16) & 0o777
                if mode and os.name != "nt":
                    os.chmod(out, mode | 0o600)
            prog.pct(100 * (n + 1) / len(infos))


def fetch(folder, version, launcher):
    info = read_release(folder)
    if version_key(version) is None:
        raise UpdateError("%r is not a version" % version)
    upd = os.path.join(folder, "update")
    os.makedirs(upd, exist_ok=True)
    if launcher:
        proc.watch_stdin(lambda m: print(m, flush=True))
    prog = Progress()
    found = None
    for rel in releases(info["repo"]):
        if not rel.get("draft") and version_key(rel.get("tag_name")) == version_key(version):
            found = rel
    a = found and package_of(found, info["platform"])
    if not a:
        raise UpdateError("release %s has no package for %s" % (version, info["platform"]))
    part, new_part, new = (os.path.join(upd, p) for p in ("download.part", "new.part", "new"))
    for p in (new_part, new, os.path.join(upd, "ready.json")):
        if os.path.isdir(p):
            shutil.rmtree(p)
        elif os.path.exists(p):
            os.remove(p)
    prog.stage(1, 2, "Downloading %s" % version)
    download(a, part, prog)
    prog.stage(2, 2, "Unpacking %s" % version)
    os.makedirs(new_part)
    unpack(part, new_part, prog)
    # the package must be a release folder of this platform, at that version
    got = read_release(new_part)
    if got["platform"] != info["platform"] or version_key(got["version"]) != version_key(version):
        raise UpdateError("the package is %s for %s, not %s for %s"
                          % (got["version"], got["platform"], version, info["platform"]))
    exe = "vf4ft-launcher.exe" if info["platform"] == "windows" else "vf4ft-launcher"
    for need in (exe, os.path.join("builder", "tools", "make_game.py"), os.path.join("builder", "tools", "update.py")):
        if not os.path.isfile(os.path.join(new_part, need)):
            raise UpdateError("the package has no %s" % need.replace(os.sep, "/"))
    with proc.installing():     # a cancel from here on waits, and then there is nothing left to cancel
        os.replace(new_part, new)
        with open(os.path.join(upd, "ready.json.part"), "w", encoding="utf-8") as f:
            json.dump({"version": got["version"], "from": info["version"],
                       "fetched": time.strftime("%Y-%m-%dT%H:%M:%S")}, f, indent=2)
            f.write("\n")
        os.replace(os.path.join(upd, "ready.json.part"), os.path.join(upd, "ready.json"))
        os.remove(part)
    print("%s is unpacked in %s" % (version, new), flush=True)
    print("@ready %s" % got["version"], flush=True)
    return 0


def main():
    a = sys.argv[1:]
    try:
        if len(a) == 2 and a[0] == "check":
            return check(os.path.abspath(a[1]))
        if len(a) in (3, 4) and a[0] == "fetch" and a[3:] in ([], ["--launcher"]):
            return fetch(os.path.abspath(a[1]), a[2], len(a) == 4)
    except proc.Cancelled:
        return proc.CANCELLED
    except (UpdateError, OSError, zipfile.BadZipFile) as e:
        print("error: %s" % e, flush=True)
        return 1
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
