"""
bof4lib -- lettura/scrittura minimale del contenitore .DAT di Breath of Fire IV
(PC / Steam app 4249150, GOG).

Formato contenitore:
    header = N entry da 16 byte: <uint32 off, uint32 size, uint32 meta1, uint32 meta2>
    Il primo offset vale la dimensione dell'header (N*16). I sub-file sono contigui,
    senza padding; l'ultimo termina esattamente a fine file.
    meta1 & 0xFF == 1  -> sub-file compresso (grafica LZHUF)
    meta2              -> indirizzo di caricamento nella RAM PS1 (residuo del porting)

Il patcher lavora a livello di SUB-FILE: sostituisce l'intero blocco individuato
dall'md5 dei byte originali. Non serve interpretare pointer table ne' control code:
la sostituzione e' byte-esatta e copre allo stesso modo testo standard, blocchi
sezionati, blocchi a tsize dispari e grafica compressa.

Questo modulo e' condiviso tra il generatore (build_patch.py) e il runtime
(apply_patch.py / installer.py). Nessuna dipendenza esterna.
"""

import struct


class ContainerError(ValueError):
    pass


class Container:
    """Contenitore .DAT: lista di sub-file, ognuno {off,size,meta1,meta2,data}."""

    def __init__(self, path_or_bytes, strict=False):
        # strict=True esige un layout contiguo senza buchi (originali PRISTINE).
        # Di default e' lenient: legge ogni sub-file dai suoi (off,size) di header,
        # tollerando i buchi/padding che il repacker lascia con l'append-in-coda.
        # build() riscrive comunque SEMPRE contiguo (il gioco legge dalla tabella
        # offset, quindi eliminare i buchi non cambia nulla per il gioco).
        if isinstance(path_or_bytes, (bytes, bytearray)):
            self.path = None
            self.raw = bytes(path_or_bytes)
        else:
            self.path = path_or_bytes
            with open(path_or_bytes, 'rb') as f:
                self.raw = f.read()
        self.entries = []
        self.contiguous = True
        self._parse(strict)

    def _parse(self, strict):
        d = self.raw
        if len(d) < 16:
            raise ContainerError('troppo piccolo (%d byte)' % len(d))
        hdr = struct.unpack_from('<I', d, 0)[0]
        if hdr == 0 or hdr % 16 or hdr > len(d):
            raise ContainerError('header non valido (%d)' % hdr)
        n = hdr // 16
        exp = hdr
        for i in range(n):
            off, size, m1, m2 = struct.unpack_from('<IIII', d, i * 16)
            if off < hdr or off + size > len(d):
                raise ContainerError('entry %d: fuori dai limiti (off %d size %d, file %d)'
                                     % (i, off, size, len(d)))
            if off != exp:
                self.contiguous = False
                if strict:
                    raise ContainerError('entry %d: offset %d, atteso %d' % (i, off, exp))
            self.entries.append({'off': off, 'size': size, 'meta1': m1, 'meta2': m2,
                                 'data': d[off:off + size]})
            exp = off + size
        if strict and exp != len(d):
            raise ContainerError('fine calcolata %d != dimensione file %d' % (exp, len(d)))

    def is_compressed(self, i):
        return (self.entries[i]['meta1'] & 0xFF) == 1

    def build(self):
        """Ricostruisce il file dai dati correnti dei sub-file (byte-esatto)."""
        n = len(self.entries)
        hdr = n * 16
        head = bytearray()
        body = bytearray()
        off = hdr
        for e in self.entries:
            head += struct.pack('<IIII', off, len(e['data']), e['meta1'], e['meta2'])
            body += e['data']
            off += len(e['data'])
        assert len(head) == hdr
        return bytes(head + body)
