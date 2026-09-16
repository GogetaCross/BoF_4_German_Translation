"""GOG exe rollback: patch a user's GOG BOF4.exe back to the working "update 6"
build, in place, from a bundled binary-delta patch.

Why: GOG updates recompile BOF4.exe, which shifts addresses and (worse) re-flows
the baked scene data so the uncensored-scene restore can't survive. GOG offers no
beta/rollback channel. So we let the user convert THEIR OWN newer exe back to the
mod-compatible update 6 with a small (~210 KB) binary patch. We never ship the exe
itself — only the diff, applied to the file the user already owns.

SCALING TO FUTURE UPDATES: there is no way to reconstruct #6 without #6's bytes
existing somewhere; a per-build delta (differences only, applied to an owned file)
is the small/defensible carrier. So each new GOG build just needs ONE more patch
targeting #6. Register it in PATCHES below (source md5 + patch filename) and ship
the patch next to this module — no code change. The tool fingerprints the user's
exe and auto-selects the matching patch.

The patches are classic BSDIFF40 (bzip2 streams), applied by `apply_bsdiff` —
pure standard library, so nothing extra needs bundling. md5 gates keep it safe:
it only touches a build it has a patch for, and verifies the result is exactly
update 6 before keeping it (else restores the backup).
"""
import os
import bz2
import hashlib

# The target every rollback produces: the mod-compatible GOG "update 6" build.
TARGET_MD5  = "7f83d1d599762a3eebc8b73a4be77d18"   # 3,634,056 bytes
TARGET_NAME = "GOG update 6"

# Every rollback-able build: its md5 → the patch that converts it to TARGET.
# ADD ONE ENTRY PER FUTURE GOG UPDATE (generate the patch with bsdiff4:
#   bsdiff4.file_diff(new_build_exe, update6_exe, "gog_rollback_<N>to6.patch")
# then drop the .patch next to this file and register it here).
PATCHES = [
    {"name": "GOG update 7", "src_md5": "4da6e2810a49666ea56fe3de47866e61",
     "patch": "gog_rollback_7to6.patch"},   # 3,625,848 bytes
]


def _offtin(b):
    """Decode bsdiff's 8-byte sign-magnitude little-endian integer."""
    y = b[7] & 0x7F
    for i in range(6, -1, -1):
        y = y * 256 + b[i]
    return -y if (b[7] & 0x80) else y


def apply_bsdiff(old: bytes, patch: bytes) -> bytes:
    """Apply a BSDIFF40 patch to `old`, return the new bytes."""
    if patch[:8] != b"BSDIFF40":
        raise ValueError("not a BSDIFF40 patch")
    ctrllen = _offtin(patch[8:16])
    datalen = _offtin(patch[16:24])
    newsize = _offtin(patch[24:32])
    if ctrllen < 0 or datalen < 0 or newsize < 0:
        raise ValueError("corrupt patch header")
    p = 32
    ctrl  = bz2.decompress(patch[p:p + ctrllen]); p += ctrllen
    diff  = bz2.decompress(patch[p:p + datalen]); p += datalen
    extra = bz2.decompress(patch[p:])
    new = bytearray(newsize)
    oldpos = newpos = ci = di = ei = 0
    while newpos < newsize:
        x = _offtin(ctrl[ci:ci + 8]); ci += 8
        y = _offtin(ctrl[ci:ci + 8]); ci += 8
        z = _offtin(ctrl[ci:ci + 8]); ci += 8
        seg = diff[di:di + x]
        ob  = old[oldpos:oldpos + x]
        new[newpos:newpos + x] = bytes((seg[k] + ob[k]) & 0xFF for k in range(x))
        di += x; newpos += x; oldpos += x
        new[newpos:newpos + y] = extra[ei:ei + y]
        ei += y; newpos += y
        oldpos += z
    return bytes(new)


def _md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def find_entry(md5: str):
    """The PATCHES entry whose source build matches this md5, or None."""
    return next((p for p in PATCHES if p["src_md5"] == md5), None)


def classify(exe_bytes: bytes):
    """Return (kind, entry):
        ('target', None)       already update 6 — nothing to do
        ('rollbackable', e)    a known newer build we have a patch for
        ('unknown', None)      not update 6 and no patch (other version / modified)
    """
    m = _md5(exe_bytes)
    if m == TARGET_MD5:
        return ("target", None)
    e = find_entry(m)
    return ("rollbackable", e) if e else ("unknown", None)


def rollback(exe_file: str, patch_dir: str, log=print) -> bool:
    """Convert a known newer GOG BOF4.exe (exe_file) to update 6, in place, using
    whichever bundled patch matches it. `patch_dir` is the folder holding the
    .patch files. Refuses anything without a matching patch; backs the original
    up to <exe>.rollback_backup; verifies the result equals update 6 (restores
    the backup on any mismatch). Returns True on success."""
    if not os.path.isfile(exe_file):
        log(f"exe not found: {exe_file}")
        return False
    old = open(exe_file, "rb").read()
    kind, entry = classify(old)
    if kind == "target":
        log(f"This exe is already {TARGET_NAME} — nothing to roll back.")
        return False
    if kind != "rollbackable":
        log(f"This exe (md5 {_md5(old)}) is not {TARGET_NAME} and no rollback "
            "patch matches it — it may be a different version (e.g. Steam) or "
            "already modified. No changes made.")
        return False

    patch_file = os.path.join(patch_dir, entry["patch"])
    if not os.path.isfile(patch_file):
        log(f"patch for {entry['name']} not found: {patch_file}")
        return False
    try:
        new = apply_bsdiff(old, open(patch_file, "rb").read())
    except Exception as e:
        log(f"patch apply failed: {e}. No changes made.")
        return False
    if _md5(new) != TARGET_MD5:
        log("Patched result did not match update 6 — aborting, no changes made.")
        return False

    backup = exe_file + ".rollback_backup"
    if not os.path.exists(backup):
        with open(backup, "wb") as f:
            f.write(old)
        log(f"Backed up your {entry['name']} exe to: {backup}")
    else:
        log(f"Backup already exists (kept): {backup}")
    with open(exe_file, "wb") as f:
        f.write(new)
    log(f"Success — BOF4.exe is now {TARGET_NAME} (the mod-compatible build), "
        f"rolled back from {entry['name']}.")
    return True
