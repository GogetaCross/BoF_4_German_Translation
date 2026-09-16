r"""
apply_patch.py -- motore che applica il pacchetto patch tedesco sul gioco.

Lavora a livello di SUB-FILE, indicizzato per md5 dei byte inglesi originali:
per ogni .DAT del gioco, ogni sub-file il cui md5 compare nel pacchetto viene
sostituito con la versione tedesca; poi il contenitore viene ricostruito contiguo.

Sicurezze (come il patch italiano):
  * BACKUP    ogni file toccato viene copiato in una cartella di backup la PRIMA
              volta soltanto (i backup esistenti non vengono sovrascritti);
  * ATOMICO   si scrive un .tmp e si rinomina;
  * RIVERIFICA il file riscritto viene riletto da disco e ogni sub-file sostituito
              deve coincidere byte per byte con quello atteso;
  * IDEMPOTENTE rilanciarlo non fa danni: gli md5 tedeschi non sono nella tabella.

Non tocca mai file diversi da quelli gia' presenti nel gioco.
"""

import hashlib
import os
import shutil
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bof4lib import Container, ContainerError  # noqa: E402

MAGIC = b'BOF4PAT1'


def load_patch(path):
    """-> {md5_bytes: (de_bytes, label)}"""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:8] != MAGIC:
        raise ValueError('non e\' un pacchetto patch valido: %s' % path)
    n, = struct.unpack_from('<I', d, 8)
    tab = {}
    p = 12
    for _ in range(n):
        md5 = d[p:p + 16]
        comp_len, raw_len = struct.unpack_from('<II', d, p + 16)
        label = d[p + 24:p + 64].rstrip(b'\0').decode('ascii', 'replace')
        p += 64
        blob = zlib.decompress(d[p:p + comp_len])
        p += comp_len
        if len(blob) != raw_len:
            raise ValueError('voce %s: lunghezza decompressa errata' % label)
        tab[md5] = (blob, label)
    return tab


def backup_once(src, backup_root, game_root):
    rel = os.path.relpath(src, game_root)
    dst = os.path.join(backup_root, rel)
    if os.path.exists(dst):
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    return True


def apply(dat_dir, tab, game_root, backup_root, dry=False, log=print):
    """Applica la tabella md5->tedesco a tutti i .DAT in dat_dir.
    Ritorna (file_scritti, sub_sostituiti, immagini/testo, errori)."""
    names = sorted(f for f in os.listdir(dat_dir) if f.upper().endswith('.DAT'))
    scritti = 0
    sub_tot = 0
    gia = 0
    errori = []
    trovati = set()

    for name in names:
        path = os.path.join(dat_dir, name)
        try:
            c = Container(path)
        except ContainerError as e:
            errori.append('%s: contenitore illeggibile (%s)' % (name, e))
            continue

        colpiti = []            # (idx, de_bytes, md5)
        for i, e in enumerate(c.entries):
            md5 = hashlib.md5(e['data']).digest()
            if md5 in tab:
                colpiti.append((i, tab[md5][0], md5))
                trovati.add(md5)
        if not colpiti:
            continue

        for i, de_bytes, md5 in colpiti:
            c.entries[i]['data'] = de_bytes
        blob = c.build()
        sub_tot += len(colpiti)

        if dry:
            scritti += 1
            continue

        backup_once(path, backup_root, game_root)
        tmp = path + '.de_tmp'
        with open(tmp, 'wb') as f:
            f.write(blob)
        os.replace(tmp, path)

        # riverifica leggendo da disco
        try:
            v = Container(path)
        except ContainerError as e:
            errori.append('%s: RIVERIFICA fallita (rilettura: %s)' % (name, e))
            continue
        if v.build() != blob:
            errori.append('%s: RIVERIFICA fallita (contenitore non stabile)' % name)
            continue
        ko = 0
        for i, de_bytes, md5 in colpiti:
            if v.entries[i]['data'] != de_bytes:
                ko += 1
        if ko:
            errori.append('%s: RIVERIFICA fallita su %d sub-file' % (name, ko))
            continue
        scritti += 1

    gia = len(tab) - len(trovati)
    return scritti, sub_tot, gia, errori
