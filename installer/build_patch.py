r"""
build_patch.py -- GENERATORE del pacchetto patch tedesco (lato sviluppatore).

Confronta le DAT inglesi originali (DAT_backup\) con quelle gia' tradotte (DAT\)
e produce un unico file patch che il runtime applichera' sul gioco dell'utente.

Idea (identica alla guardia grafica del patch italiano, estesa a TUTTO il testo):
ogni sub-file che cambia viene indicizzato per **md5 dei byte ORIGINALI inglesi**.
Quell'md5 e' insieme il modo di ritrovare il blocco nel gioco dell'utente e la
garanzia di sicurezza:
  * un sub-file che non e' esattamente l'originale inglese non ha quell'md5 -> non
    viene toccato;
  * rilanciare la patch su un gioco gia' tradotto non fa nulla (gli md5 non
    corrispondono piu');
  * blocchi identici che compaiono in piu' .DAT hanno lo stesso md5 -> memorizzati
    UNA volta sola, applicati ovunque (dedup automatica).

Copre allo stesso modo testo standard, blocchi sezionati (AB000_00[1]/INIT[7]),
blocchi a tsize dispari (AREAM005...) e grafica compressa: si lavora a livello di
sub-file, senza interpretare pointer table o control code.

Uso:
    python build_patch.py                 usa DAT_backup\ (EN) e DAT\ (DE)
    python build_patch.py --en DIR --de DIR --out FILE
    python build_patch.py --only-text     salta i sub-file compressi (grafica)

Output di default: de_patch\payload\patch_de.bof4  +  payload\manifesto.sha256
"""

import argparse
import hashlib
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'runtime'))
from bof4lib import Container, ContainerError  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
GAME_ROOT = os.path.dirname(HERE)                 # de_patch\ vive sotto la radice del gioco
MAGIC = b'BOF4PAT1'


def _dat_names(d):
    return sorted(f for f in os.listdir(d) if f.upper().endswith('.DAT'))


def build(en_dir, de_dir, out_path, only_text=False, verbose=True):
    en_names = set(_dat_names(en_dir))
    de_names = set(_dat_names(de_dir))
    comuni = sorted(en_names & de_names)
    if verbose:
        print('DAT inglesi : %d   tedeschi: %d   in comune: %d'
              % (len(en_names), len(de_names), len(comuni)))

    patch = {}          # md5_en(bytes) -> (de_bytes, label)
    n_diff = 0
    n_dedup = 0
    n_gfx = 0
    saltati = []
    manifest = []       # (sha256_en, name)

    # DEMO2.DAT is shipped as a bsdiff (US/JP title choice, see de_patch/inno);
    # excluded here so the md5 text-patch never touches the title records.
    EXCLUDE = {'DEMO2.DAT'}
    for name in comuni:
        if name.upper() in EXCLUDE:
            saltati.append('%s: via bsdiff (Titelbildschirm-Wahl)' % name)
            continue
        try:
            ce = Container(os.path.join(en_dir, name))
            cd = Container(os.path.join(de_dir, name))
        except ContainerError as e:
            saltati.append('%s: %s' % (name, e))
            continue
        # il manifesto registra lo stato ORIGINALE inglese (integrita')
        manifest.append((hashlib.sha256(ce.raw).hexdigest(), name))

        if len(ce.entries) != len(cd.entries):
            saltati.append('%s: numero sub-file diverso (%d vs %d)'
                           % (name, len(ce.entries), len(cd.entries)))
            continue

        for i, (ee, ed) in enumerate(zip(ce.entries, cd.entries)):
            if ee['data'] == ed['data']:
                continue                                  # invariato
            compressed = (ee['meta1'] & 0xFF) == 1
            if compressed:
                n_gfx += 1
                if only_text:
                    continue
            md5 = hashlib.md5(ee['data']).digest()
            n_diff += 1
            if md5 in patch:
                n_dedup += 1
                continue
            label = ('%s[%d]%s' % (name, i, ':gfx' if compressed else ''))[:40]
            patch[md5] = (ed['data'], label)

    # scrittura pacchetto: MAGIC, count, poi per voce
    #   md5(16) + comp_len(u32) + raw_len(u32) + label(40s) + zlib(de_bytes)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('<I', len(patch)))
        for md5, (de_bytes, label) in patch.items():
            comp = zlib.compress(de_bytes, 9)
            f.write(md5)
            f.write(struct.pack('<II', len(comp), len(de_bytes)))
            f.write(label.encode('ascii', 'replace').ljust(40, b'\0'))
            f.write(comp)

    man_path = os.path.join(os.path.dirname(out_path), 'manifesto.sha256')
    with open(man_path, 'w', encoding='utf-8') as f:
        f.write('# SHA-256 dei .DAT originali inglesi (stato prima della patch)\n')
        for h, name in manifest:
            f.write('%s  %s\n' % (h, name))

    if verbose:
        print('sub-file modificati    : %d  (di cui grafica: %d)' % (n_diff, n_gfx))
        print('voci nel pacchetto     : %d  (dedup: %d identici in piu\' file)'
              % (len(patch), n_dedup))
        print('pacchetto              : %s  (%d byte)'
              % (out_path, os.path.getsize(out_path)))
        print('manifesto              : %s  (%d file)' % (man_path, len(manifest)))
        if saltati:
            print('saltati (%d):' % len(saltati))
            for s in saltati[:20]:
                print('  ! ' + s)
    return len(patch)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Genera il pacchetto patch tedesco.')
    ap.add_argument('--en', default=os.path.join(GAME_ROOT, 'DAT_backup'),
                    help='cartella .DAT inglesi originali')
    ap.add_argument('--de', default=os.path.join(GAME_ROOT, 'DAT'),
                    help='cartella .DAT tedesche gia\' costruite')
    ap.add_argument('--out', default=os.path.join(HERE, 'payload', 'patch_de.bof4'))
    ap.add_argument('--only-text', action='store_true',
                    help='non includere i sub-file grafici compressi')
    a = ap.parse_args(argv)
    for d in (a.en, a.de):
        if not os.path.isdir(d):
            raise SystemExit('cartella inesistente: %s' % d)
    build(a.en, a.de, a.out, a.only_text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
