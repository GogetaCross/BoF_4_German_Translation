"""
Portable NA-uncensor DAT restoration for the BoF4 localization pipeline.

The North-American release cut several scenes by collapsing whole runs of the
area DAT's message-offset table onto a single stub string, and by dropping the
uncut dialogue from the string pool. The runtime scene choreography is restored
by the d3d9 hook (censor_fix_aream027 etc.); THIS module restores the missing
*dialogue text* so the boxes read correctly.

Crucially it does so WITHOUT shipping a modified DAT. The original US strings are
baked in below (byte-exact, control codes included). `apply()` materialises them
into a pristine (censored) DAT *in memory* — appending the strings to the entry
pool and repointing the offset table — so anyone who dumps their OWN pristine
files gets the uncut dialogue automatically. It is idempotent (a no-op on a DAT
that already has the strings) and version-tolerant (only touches a DAT whose
restore slots are still collapsed onto one offset, i.e. genuinely censored).

Used by:
  * extract_text.py  — pre-pass after reading each DAT, so text_dump shows the lines.
  * repack_text.py   — pre-pass on the source DAT, so the live DAT gets the lines
                       (translations for these slots then apply normally).

Determinism: the censored entry pool ends at a fixed size, so appending the baked
strings in slot order always lands them at the same in-entry offsets. Those are
the offsets the editor's UNCENSOR_ENTRIES seal is keyed on — keep the two in sync.
"""
import struct

# base (DAT name, no extension) -> entry_index -> [(slot, raw_bytes), ...]
# in APPEND ORDER. raw_bytes are the exact stored bytes WITHOUT the trailing
# null (apply() adds it). Control codes are inline: 0x01 newline, 0x02 paragraph,
# 0x0B small-pause, 0x14 <flag> <id> = 2-byte portrait, etc.
#
# AREAM027 — sailors / Ursula ship scene (area 416). Slots 0x46..0x4E; 0x4F
# ("...you win.") already exists in the censored pool and is left untouched.
# 0x4D is Ursula's "You just want to see my butt, right?" — its portrait code is
# the correct 2-param [14:83:44] (the raw PSX stub was a truncated 1-param
# [14:44], which made the renderer eat the following character).
UNCENSOR_RESTORE = {
    "AREAM027": {
        8: [
            (0x46, bytes.fromhex(
                "0c06148242224e6f74207965742e2e2e0154686572652773206f6e65206d6f72"
                "65017468696e672e2e2e22")),
            (0x47, bytes.fromhex(
                "0c06148242225768656e2061207361696c6f72206d6573736573017468696e67"
                "732075702e2e2e0249276d20737570706f73656420746f01736d61636b206869"
                "6d206f6e207468652062757474017769746820746869732e22")),
            (0x48, bytes.fromhex(
                "0c0614824222496e206f7468657220776f7264732c01696620796f75277265206e"
                "6f7420726561647920746f017765617220796f75722070616e74696573206174"
                "026869702d6c6576656c20616c6c207468652074696d652e2e2e017765277265"
                "206e6f7420676f6e6e61206c657420796f75016f6e2074686520736869702e22")),
            (0x49, bytes.fromhex(
                "0c0614824222536f2c20686f772061626f757420796f752073686f7701757320"
                "796f757220627574743f22")),
            (0x4A, bytes.fromhex(
                "140a0522492e2e2e0b492e2e2e200b0149206861766520746f2073686f772079"
                "6f75016d792062757474213f22")),
            (0x4B, bytes.fromhex(
                "0c061480432259656168212055732067757973206c6f766520736f6d6501627574"
                "74732122")),
            (0x4C, bytes.fromhex(
                "0c06148242225365652c20796f752063616e277420646f2069742c0163616e20"
                "79613f024a757374206769766520757020616e6420676574017573656420746f"
                "2073746179696e2720686572652e22")),
            (0x4D, bytes.fromhex(
                "0c0514834422596f75206a7573742077616e7420746f20736565016d79206275"
                "74742c2072696768743f22")),
            (0x4E, bytes.fromhex(
                "0c0614824222412e2e2e0b416c7269676874207468656e2c204920736565017468"
                "617420796f752074776f2061726520766572790164657465726d696e65642e2e"
                "2e024e6f772070757420796f75722070616e7473016f6e2e2e2e22")),
        ],
    },
    # AREAD145 — "3rd scene" / girls' bath (area 277). The NA release cut the whole
    # scene (runtime restored by the d3d9 hook flag censor_fix_aread145); its 11
    # dialogue slots 0x43..0x4D were collapsed onto one stub in the DAT. English
    # recovered from the Navarchos uncut patch (BIN/WORLD/AREAD145.EMI) and remapped
    # PSX->PC (0x17->0x14 etc.). Slots 0x44/0x47/0x4D each store a portrait head
    # (0C 05/02 14 41) + an inline 00 + the text; the engine reads through the 00.
    "AREAD145": {
        7: [
            (0x43, bytes.fromhex(
                # JP switches portrait mid-box: page1 [14:41] "I..." -> [02] page
                # break -> [14:07] "smell like fish." The uncut patch flattened it
                # to a single [14:07] + [0B] pause; restored to match JP fidelity.
                "0c0514410122492e2e2e02140701736d656c6c206c696b6520666973682e22")),
            (0x44, bytes.fromhex(
                "0c051441002249742773206265656e2061206c6f6e672074696d650173696e6365"
                "204927766520686164206120626174682e02497420636f756c64206265206e6963"
                "6520746f20676f01666f722061206469702e22")),
            (0x45, bytes.fromhex(
                "0c0514070122556d6d2e2e2e20796f752063616e2774206a757374016e6f642061"
                "6c6f6e672c2004002e02140f0149276d20676f696e6720746f2074616b65206101"
                "626174682e20506c656173652c2067697665206d652001736f6d6520707269766163792122")),
            (0x46, bytes.fromhex(
                "0c05140701225468616e6b20796f752c2004002e22")),
            (0x47, bytes.fromhex(
                "0c0214410022556d2c20636f756c6420796f7520706c65617365017374616e6420"
                "677561726420616e64206d616b650173757265206e6f206f6e6502636f6d657320"
                "6f7665723f014920646f6e27742077616e7420736f6d656f6e6501746f20736565206d652e22")),
            (0x48, bytes.fromhex(
                "0c02149041224168682c206974277320736f206e69636520616e6401636f6c642e"
                "0204042c20636f6d6520616e64206a6f696e016d652122")),
            (0x49, bytes.fromhex(
                "0c021491412241682c2074686973206665656c7320736f01676f6f642122")),
            (0x4A, bytes.fromhex(
                "0c021492412249206e6576657220657870656374656420796f75276401626520736f"
                "2062656175746966756c2c0104042e2e2e22")),
            (0x4B, bytes.fromhex(
                "0c0214130322486d70682e2054686572652773206d6f726520746f01626561757479"
                "207468616e2061016269672063686573742c2079276b6e6f772e22")),
            (0x4C, bytes.fromhex(
                "0c021494412204002120592d0b596f752073747570696401706572766572742122")),
            (0x4D, bytes.fromhex(
                "0c0514410022576520776572652061626c6520746f2066696e640166726573682077"
                "617465722c20616e64206974026c6f6f6b73206c696b652074686572652773206101"
                "706c6163652077686572652077652063616e01636174636820666973682c02736f20"
                "6974206c6f6f6b73206c696b65017765276c6c20626520616c6c20726967687401666f722061207768696c652e22")),
        ],
    },
    # AREAD157 — the Emperor confrontation (area 289). This was primarily a VIOLENCE
    # cut (Fou-Lu's decapitation of the Emperor + throne ascension; runtime-restored
    # by the d3d9 hook flag censor_fix_aread157). The cut ALSO dropped Yuna's one
    # dialogue box: the NA build collapsed message slot 0x1D onto its sibling 0x1E
    # ("Thou art come at last..."), so the box at scene-selector 0x16 wrongly
    # repeated Fou-Lu's line instead of Yuna's commentary on the god-slaying weapon.
    # English recovered from the Navarchos uncut patch (BIN/WORLD/AREAD157.EMI id
    # 0x1C) with JP's 2-param speaker header [14:02:02] (AREAD157.DAT rec#9 id 0x1D
    # in the JP build) and the [09:56:01] weapon-name insert preserved. Unlike the
    # multi-slot collapses above this is a SINGLE-slot re-point (0x1D was a dup of a
    # DISTINCT sibling, not part of a collapsed run) — apply()'s idempotency falls
    # back to a per-slot baked-string compare for this shape (see below).
    "AREAD157": {
        9: [
            (0x1D, bytes.fromhex(
                "1402022257656c6c21204a757374206173206578706563746564016f66207468"
                "65200956012e024576656e20696620616e20616d617465757201757365732069"
                "742c2069742063616e20656173696c790164616d616765206120676f642e22")),
        ],
    },
}


# Extra text_sha1 fingerprints (pristine_guard.dat_text_hash) that are VALID
# repack sources for a patched DAT, beyond the manifest's single pristine hash.
# Because apply() is idempotent, BOTH the pre-patch (censored) and post-patch
# (uncensored) states of a patched DAT are legitimate sources. Listing both lets
# the pristine gate accept either WITHOUT the blunt "Use anyway → re-learn the
# whole manifest" step — while a translated/other-version DAT (any other hash)
# still trips the gate. Keyed by DAT filename (upper-case).
ACCEPTED_TEXT_SHA1 = {
    "AREAM027.DAT": {
        "8cdea56cfc2176892e9e65aa1cfd1d4488576943",  # pristine censored (US)
        "c28b1deee12777b5a2ef09e176dc27537decbf16",  # uncensor materialised
    },
    "AREAD145.DAT": {
        "26acf61b2cfe1492d3447f6c2777ce7c152bc435",  # pristine censored (US)
        "488c28c7fd7aaaf30b0546244b07dd187cc8c154",  # uncensor materialised (0x43 portrait-switch)
    },
    "AREAD157.DAT": {
        "ec368695155af947f6640134ee8164451f085311",  # pristine censored (US)
        "a758cdd35643bbfdfd3abde83362362e30988b4c",  # uncensor materialised (Yuna id 0x1D)
    },
}


def accepted_text_sha1(name):
    """Set of extra-accepted text_sha1 for a DAT filename (empty if none)."""
    import os
    return ACCEPTED_TEXT_SHA1.get(os.path.basename(str(name)).upper(), set())


def _basekey(name):
    """'AREAM027.DAT' / 'aream027' / path -> 'AREAM027'."""
    import os
    b = os.path.basename(str(name))
    if "." in b:
        b = b[:b.rindex(".")]
    return b.upper()


def has_patch(name):
    """True if a baked restore exists for this DAT name."""
    return _basekey(name) in UNCENSOR_RESTORE


def apply(name, buf):
    """Return DAT bytes with the baked uncensor dialogue materialised.

    Idempotent: if a base has no patch, or its restore slots are already distinct
    (already uncensored), that base is left untouched. Accepts bytes/bytearray;
    always returns immutable bytes.
    """
    spec = UNCENSOR_RESTORE.get(_basekey(name))
    if not spec:
        return bytes(buf)
    d = bytearray(buf)
    if len(d) < 16:
        return bytes(d)
    for entry_idx, entries in spec.items():
        e_base = 0x10 + entry_idx * 16
        if e_base + 16 > len(d):
            continue
        off, size, f2, _f3 = struct.unpack_from("<IIII", d, e_base)
        if f2 != 0 or off + size > len(d):
            continue                      # not a text entry / malformed
        tbl_off = off

        # Robust idempotency (works for single-slot re-points too): if every
        # restore slot already stores its exact baked string, this base is done.
        def _stored(slot):
            so = struct.unpack_from("<H", d, tbl_off + slot * 2)[0]
            p = q = off + so
            while q < off + size and d[q] != 0:
                q += 1
            return bytes(d[p:q])
        if all(_stored(slot) == bytes(raw) for slot, raw in entries):
            continue                      # already materialised -> no-op

        # Censored tell for the MULTI-slot collapses (AREAM027/AREAD145): every
        # restore slot points at the SAME stub offset. A single-slot re-point
        # (AREAD157: 0x1D duplicated a DISTINCT sibling) skips this guard and
        # relies on the baked-string compare above for idempotency.
        cur_offs = [struct.unpack_from("<H", d, tbl_off + s * 2)[0]
                    for s, _ in entries]
        if len(entries) > 1 and len(set(cur_offs)) != 1:
            continue                      # already uncensored -> no-op
        # Append all baked strings at the current end of the entry pool.
        blob = bytearray()
        newmap = {}
        cur = size
        for slot, raw in entries:
            newmap[slot] = cur
            s = bytes(raw) + b"\x00"
            blob += s
            cur += len(s)
        ins_at = off + size
        d[ins_at:ins_at] = blob
        for slot, new_off in newmap.items():
            struct.pack_into("<H", d, tbl_off + slot * 2, new_off)
        # Grow this entry's TOC size; shift every later entry's file offset.
        delta = len(blob)
        n = (struct.unpack_from("<I", d, 0)[0] - 16) // 16
        for idx in range(n):
            b = 0x10 + idx * 16
            eo, es = struct.unpack_from("<II", d, b)
            if idx == entry_idx:
                struct.pack_into("<I", d, b + 4, es + delta)
            elif eo > off:
                struct.pack_into("<I", d, b, eo + delta)
    return bytes(d)
