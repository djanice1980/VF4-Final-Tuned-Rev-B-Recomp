#!/usr/bin/env python3
"""Add the game to Steam as a non-Steam game (the builder's "add to Steam").

usage:
  python3 -I tools/steam_shortcut.py add EXE [--name NAME] [--proton TOOL] [--steam DIR] [--dry-run]
  python3 -I tools/steam_shortcut.py list [--steam DIR]
  python3 -I tools/steam_shortcut.py play EXE [--name NAME]    (starts the shortcut through Steam)

DIR defaults to ~/.steam/steam on Linux and to the SteamPath in the
registry on Windows.

Under Wine on Linux (the Windows launcher, run by plain Wine or by Proton)
the tool works on the real Steam of the Linux host, not on Wine's: it finds
Steam's folder in your Linux home (~/.steam/steam, ~/.local/share/Steam or
the Flatpak's) through Wine's drives, sees whether Steam runs in /proc, and
writes the entry as the Linux tool does (the exe's Linux path, Proton).

For every Steam user in DIR/userdata it adds (or updates, matched by the
app name) an entry in config/shortcuts.vdf. For a Windows .exe on Linux it
also sets the compatibility tool for that entry in DIR/config/config.vdf
(default proton_experimental), as Steam's own "Force the use of a specific
Steam Play compatibility tool" does.

Safety:
  - Steam must not be running (it rewrites these files on exit); the tool
    refuses otherwise, with exit code 3 (the launcher's "close Steam" note).
  - Each file is read, re-serialized and compared byte for byte with the
    original before anything is changed; if the parser does not reproduce it
    exactly, the tool stops and changes nothing.
  - The originals are copied beside themselves as *.vf4ft-backup-<time>.
  - Writes go to a temporary file that is then renamed over the original.
"""
import argparse
import os
import posixpath
import shutil
import struct
import subprocess
import sys
import time
import zlib

# ---------------------------------------------------------------- binary VDF
# shortcuts.vdf: 0x00 name map ... 0x08 end; 0x01 name string; 0x02 name int32.

def _cstr(data, i):
    j = data.index(b"\0", i)
    return data[i:j].decode("utf-8", "surrogateescape"), j + 1


def bvdf_parse(data):
    def read_map(i):
        items = []
        while True:
            t = data[i]
            i += 1
            if t == 0x08:
                return items, i
            key, i = _cstr(data, i)
            if t == 0x00:
                val, i = read_map(i)
                items.append((key, val))
            elif t == 0x01:
                val, i = _cstr(data, i)
                items.append((key, val))
            elif t == 0x02:
                items.append((key, struct.unpack_from("<i", data, i)[0]))
                i += 4
            else:
                raise ValueError(f"shortcuts.vdf: unknown type {t:#x} at {i - 1}")
    root, i = read_map(0)
    if i != len(data):
        raise ValueError(f"shortcuts.vdf: {len(data) - i} bytes after the end")
    return root


def bvdf_dump(items):
    out = bytearray()

    def write_map(items):
        for key, val in items:
            k = key.encode("utf-8", "surrogateescape") + b"\0"
            if isinstance(val, list):
                out.extend(b"\x00" + k)
                write_map(val)
            elif isinstance(val, int):
                out.extend(b"\x02" + k + struct.pack("<i", val))
            else:
                out.extend(b"\x01" + k + val.encode("utf-8", "surrogateescape") + b"\0")
        out.append(0x08)
    write_map(items)
    return bytes(out)

# ------------------------------------------------------------------ text VDF
# config.vdf: "key" "value" / "key" { ... }, tabs, backslash escapes kept as written.

def tvdf_parse(text):
    toks = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c == '"':
            j = i + 1
            while text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            toks.append(("s", text[i + 1:j]))   # raw: escapes left as they are
            i = j + 1
        elif c in "{}":
            toks.append((c, c))
            i += 1
        elif text.startswith("//", i):
            raise ValueError("config.vdf: comments are not expected")
        else:
            raise ValueError(f"config.vdf: unexpected {c!r} at {i}")
    pos = 0

    def read_map(top):
        nonlocal pos
        items = []
        while pos < len(toks):
            t, v = toks[pos]
            if t == "}":
                if top:
                    raise ValueError("config.vdf: unbalanced }")
                pos += 1
                return items
            key = v
            pos += 1
            t2, v2 = toks[pos]
            pos += 1
            if t2 == "{":
                items.append((key, read_map(False)))
            else:
                items.append((key, v2))
        if not top:
            raise ValueError("config.vdf: missing }")
        return items
    return read_map(True)


def tvdf_dump(items, depth=0):
    out = []
    tab = "\t" * depth
    for key, val in items:
        if isinstance(val, list):
            out.append(f'{tab}"{key}"\n{tab}{{\n')
            out.append(tvdf_dump(val, depth + 1))
            out.append(f"{tab}}}\n")
        else:
            out.append(f'{tab}"{key}"\t\t"{val}"\n')
    return "".join(out)


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')

# -------------------------------------------------------------------- helpers

def get(items, key):
    for k, v in items:
        if k == key:
            return v
    return None


def get_or_add_map(items, key):
    v = get(items, key)
    if v is None:
        v = []
        items.append((key, v))
    return v


def shortcut_appid(exe_field, name):
    # A stable id for the shortcut: CRC-32 of the quoted exe and the name, top bit set (the
    # scheme shortcut tools use). Current Steam gives shortcuts it creates other ids, but it
    # uses whatever id the file holds, and CompatToolMapping is keyed by that same id.
    return (zlib.crc32((exe_field + name).encode("utf-8")) & 0xFFFFFFFF) | 0x80000000


EXIT_STEAM_RUNNING = 3

# ------------------------------------------------- the Windows build under Wine
# Wine's own processes, registry and "Steam" (Proton's stand-in) are not the
# real Steam's, so under Wine on Linux the tool goes to the host: Wine's
# kernel32 turns its paths into Linux ones and back (wine_get_unix_file_name,
# wine_get_dos_file_name, which winepath uses), and /proc is read through them.

def wine_host():
    """The host system's name under Wine ('Linux'), else None."""
    if os.name != "nt":
        return None
    import ctypes
    try:
        f = ctypes.WinDLL("ntdll").wine_get_host_version
    except (OSError, AttributeError):
        return None
    f.restype = None
    f.argtypes = [ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_char_p)]
    sysname, release = ctypes.c_char_p(), ctypes.c_char_p()
    f(ctypes.byref(sysname), ctypes.byref(release))
    return (sysname.value or b"").decode("utf-8", "replace")


def _wine_kernel32():
    import ctypes
    k = ctypes.WinDLL("kernel32")
    k.wine_get_unix_file_name.restype = ctypes.c_void_p
    k.wine_get_unix_file_name.argtypes = [ctypes.c_wchar_p]
    k.wine_get_dos_file_name.restype = ctypes.c_void_p
    k.wine_get_dos_file_name.argtypes = [ctypes.c_char_p]
    k.GetProcessHeap.restype = ctypes.c_void_p
    k.HeapFree.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
    return k


def unix_path(dos):
    """The Linux path of a Windows path, under Wine."""
    import ctypes
    k = _wine_kernel32()
    p = k.wine_get_unix_file_name(dos)
    if not p:
        raise SystemExit(f"{dos}: Wine gives no Linux path for it")
    try:
        return ctypes.string_at(p).decode("utf-8", "surrogateescape")
    finally:
        k.HeapFree(k.GetProcessHeap(), 0, p)


def dos_path(unix):
    """The Windows path Wine gives a Linux path, or None."""
    import ctypes
    k = _wine_kernel32()
    p = k.wine_get_dos_file_name(unix.encode("utf-8", "surrogateescape"))
    if not p:
        return None
    try:
        return ctypes.wstring_at(p)
    finally:
        k.HeapFree(k.GetProcessHeap(), 0, p)


def host_home():
    h = os.environ.get("HOME", "")
    if h.startswith("/"):
        return h
    w = os.environ.get("WINEHOMEDIR", "")          # set by Wine: \??\Z:\home\name
    if w:
        return unix_path(w[4:] if w.startswith("\\??\\") else w)
    raise SystemExit("cannot find your Linux home folder from Wine (no HOME or WINEHOMEDIR)")


def host_steam_dir():
    home = host_home()
    tried = []
    for d in ("/.steam/steam", "/.local/share/Steam", "/.var/app/com.valvesoftware.Steam/.local/share/Steam"):
        w = dos_path(home + d)
        tried.append(home + d)
        if w and os.path.isdir(os.path.join(w, "userdata")):
            return w
    raise SystemExit("no Steam folder found in your Linux home (" + ", ".join(tried) + ")")


def host_steam_running():
    """Whether the Linux host's Steam runs: a process named steam (as pgrep -x steam), or
    Steam's own launch variables (a launcher started by Steam, whose /proc may not show it)."""
    if any(os.environ.get(v) for v in ("SteamAppId", "SteamGameId", "SteamClientLaunch")):
        return True
    proc = dos_path("/proc")
    if not proc:
        raise SystemExit("cannot see the Linux processes from Wine: close Steam, then try again")
    for pid in os.listdir(proc):
        if not pid.isdigit():
            continue
        try:
            with open(os.path.join(proc, pid, "comm"), "rb") as f:
                if f.read().strip() == b"steam":
                    return True
        except OSError:
            pass
    return False


def steam_running():
    if wine_host() == "Linux":
        return host_steam_running()
    if os.name == "nt":
        r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq steam.exe", "/NH"], capture_output=True, text=True,
                           creationflags=0x08000000)       # CREATE_NO_WINDOW
        return "steam.exe" in r.stdout.lower()
    r = subprocess.run(["pgrep", "-x", "steam"], capture_output=True)
    return r.returncode == 0


def default_steam_dir():
    if wine_host() == "Linux":
        return host_steam_dir()
    if os.name == "nt":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
                return os.path.normpath(winreg.QueryValueEx(k, "SteamPath")[0])
        except OSError:
            return r"C:\Program Files (x86)\Steam"
    return os.path.realpath(os.path.expanduser("~/.steam/steam"))


def cmd_play(a):
    """Starts the shortcut through Steam (so Steam Input and, on Linux, Proton apply)."""
    exe = os.path.abspath(a.exe)
    appid = shortcut_appid(f'"{exe}"', a.name)
    url = "steam://rungameid/%d" % ((appid << 32) | 0x02000000)
    print(url)
    if os.name == "nt":
        os.startfile(url)
    else:
        for opener in (["steam", url], ["xdg-open", url]):
            if shutil.which(opener[0]):
                subprocess.Popen(opener, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                return
        raise SystemExit("neither steam nor xdg-open is on PATH")


def backup(path, stamp):
    dst = f"{path}.vf4ft-backup-{stamp}"
    shutil.copy2(path, dst)
    return dst


def write_atomic(path, data):
    tmp = path + ".vf4ft-tmp"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def load_shortcuts(path):
    if not os.path.exists(path):
        return None, [("shortcuts", [])]
    data = open(path, "rb").read()
    root = bvdf_parse(data)
    if bvdf_dump(root) != data:
        raise SystemExit(f"{path}: could not reproduce the file exactly; nothing changed")
    return data, root


def load_config(path):
    text = open(path, "rb").read().decode("utf-8", "surrogateescape")
    root = tvdf_parse(text)
    if tvdf_dump(root) != text:
        raise SystemExit(f"{path}: could not reproduce the file exactly; nothing changed")
    return text, root

# ------------------------------------------------------------------ commands

def cmd_list(steam):
    for uid in sorted(os.listdir(os.path.join(steam, "userdata"))):
        path = os.path.join(steam, "userdata", uid, "config", "shortcuts.vdf")
        if not os.path.exists(path):
            continue
        _, root = load_shortcuts(path)
        for idx, entry in get(root, "shortcuts") or []:
            appid = get(entry, "appid") & 0xFFFFFFFF
            print(f"user {uid}  #{idx}  appid {appid}  {get(entry, 'AppName')!r}  {get(entry, 'Exe')}")


def cmd_add(a):
    exe = os.path.abspath(a.exe)
    if not os.path.isfile(exe):
        raise SystemExit(f"{exe}: no such file")
    if steam_running():
        print("error: Steam is running: close it first (Steam > Exit), then try again", flush=True)
        sys.exit(EXIT_STEAM_RUNNING)
    # the Linux host's Steam, from Wine: the entry as the Linux tool writes it
    linux = sys.platform.startswith("linux") or wine_host() == "Linux"
    if os.name == "nt" and linux:
        exe = unix_path(exe)
    exe_field = f'"{exe}"'
    # as Steam itself writes them: quoted with a backslash on Windows, bare on Linux
    start_dir = posixpath.dirname(exe) + "/" if linux else f'"{os.path.dirname(exe)}\\"'
    appid = shortcut_appid(exe_field, a.name)
    windows_exe = exe.lower().endswith(".exe")
    stamp = time.strftime("%Y%m%d-%H%M%S")

    users = sorted(u for u in os.listdir(os.path.join(a.steam, "userdata")) if u.isdigit() and u != "0")
    if not users:
        raise SystemExit(f"no Steam users under {a.steam}/userdata (start Steam and sign in once)")

    for uid in users:
        path = os.path.join(a.steam, "userdata", uid, "config", "shortcuts.vdf")
        old, root = load_shortcuts(path)
        shortcuts = get_or_add_map(root, "shortcuts")
        entry = None
        for _, e in shortcuts:
            if get(e, "AppName") == a.name:
                entry = e
                break
        fields = [
            ("appid", appid - (1 << 32)),   # stored as a signed int32
            ("AppName", a.name), ("Exe", exe_field), ("StartDir", start_dir),
            ("icon", ""), ("ShortcutPath", ""), ("LaunchOptions", a.launch_options),
            ("IsHidden", 0), ("AllowDesktopConfig", 1), ("AllowOverlay", 1), ("OpenVR", 0),
            ("Devkit", 0), ("DevkitGameID", ""), ("DevkitOverrideAppID", 0), ("LastPlayTime", 0),
            ("FlatpakAppID", ""), ("sortas", ""), ("tags", []),
        ]
        if entry is None:
            shortcuts.append((str(len(shortcuts)), fields))
            what = "added"
        else:
            keep = dict(entry)
            entry[:] = [(k, keep["LastPlayTime"] if k == "LastPlayTime" and "LastPlayTime" in keep else v)
                        for k, v in fields]
            what = "updated"
        new = bvdf_dump(root)
        print(f"user {uid}: {what} {a.name!r} appid {appid} -> {path}")
        if not a.dry_run and new != old:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            if old is not None:
                print(f"  backup {backup(path, stamp)}")
            write_atomic(path, new)

    if windows_exe and linux and a.proton:
        path = os.path.join(a.steam, "config", "config.vdf")
        old, root = load_config(path)
        node = root
        for key in ("InstallConfigStore", "Software", "Valve", "Steam"):
            node = get(node, key)
            if node is None:
                raise SystemExit(f"{path}: no {key} section; nothing changed")
        mapping = get_or_add_map(node, "CompatToolMapping")
        tool = [("name", esc(a.proton)), ("config", ""), ("priority", "250")]
        for i, (k, v) in enumerate(mapping):
            if k == str(appid):
                mapping[i] = (k, tool)
                break
        else:
            mapping.append((str(appid), tool))
        new = tvdf_dump(root)
        print(f"compat tool {a.proton} for appid {appid} -> {path}")
        if not a.dry_run and new != old:
            print(f"  backup {backup(path, stamp)}")
            write_atomic(path, new.encode("utf-8", "surrogateescape"))
    if a.dry_run:
        print("dry run: nothing written")


def main():
    default_steam = default_steam_dir()
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    pa = sub.add_parser("add")
    pa.add_argument("exe")
    pa.add_argument("--name", default="Virtua Fighter 4 Final Tuned")
    pa.add_argument("--proton", default="proton_experimental",
                    help="compatibility tool for a .exe on Linux ('' for none)")
    pa.add_argument("--launch-options", default="")
    pa.add_argument("--steam", default=default_steam)
    pa.add_argument("--dry-run", action="store_true")
    pl = sub.add_parser("list")
    pl.add_argument("--steam", default=default_steam)
    pp = sub.add_parser("play")
    pp.add_argument("exe")
    pp.add_argument("--name", default="Virtua Fighter 4 Final Tuned")
    a = p.parse_args()
    if a.cmd == "list":
        cmd_list(a.steam)
    elif a.cmd == "play":
        cmd_play(a)
    else:
        cmd_add(a)


if __name__ == "__main__":
    main()
