"""Extract Qt rcc resources embedded in an ELF binary (no symbols needed).

1. Find rcc name entries (u16 len, u32 qt_hash, utf16be chars) validated by hash.
2. Group into contiguous name blobs; find the tree table that references them.
3. Locate the data blob by requiring several files' size headers / magic to line up.
"""
import struct, sys, os, re, zlib
import zstandard


def qt_hash(u):
    h = 0
    for i in range(0, len(u), 2):
        c = (u[i] << 8) | u[i + 1]
        h = ((h << 4) + c) & 0xffffffff
        g = h & 0xf0000000
        if g:
            h ^= g >> 23
            h &= ~g & 0xffffffff
    return h


def find_names(b):
    names = {}
    for m in re.finditer(rb'(?=\x00[\x01-\xff][\x00-\xff]{4}\x00[\x20-\x7e])', b, re.S):
        o = m.start()
        ln, hs = struct.unpack_from('>HI', b, o)
        u = b[o + 6:o + 6 + ln * 2]
        if len(u) != ln * 2:
            continue
        if qt_hash(u) == hs:
            try:
                names[o] = u.decode('utf-16-be')
            except UnicodeDecodeError:
                pass
    return names


def group(names):
    offs = sorted(names)
    blobs, cur = [], [offs[0]]
    for o in offs[1:]:
        p = cur[-1]
        if o == p + 6 + len(names[p]) * 2:
            cur.append(o)
        else:
            blobs.append(cur)
            cur = [o]
    blobs.append(cur)
    return blobs


def find_tree(b, rel):
    """Yield candidate tree roots. Root: name 0, flags 2, first child 1."""
    keys = sorted(rel)
    seen = set()
    for k in keys[-3:]:
        pat = struct.pack('>I', k)
        idx = b.find(pat)
        while idx != -1:
            for back in range(0, 22 * 6000, 22):
                r = idx - back
                if r < 0:
                    break
                no, fl = struct.unpack_from('>IH', b, r)
                if no not in rel or fl not in (0, 1, 2, 4):
                    break
                if no == 0 and fl == 2:
                    cnt, first = struct.unpack_from('>II', b, r + 6)
                    if first == 1 and 0 < cnt < 6000 and r not in seen:
                        seen.add(r)
                        yield r
                        break
            idx = b.find(pat, idx + 1)


def walk_tree(b, tree, rel):
    files = []
    seen = set()

    def walk(i, prefix):
        if i in seen or len(seen) > 20000:
            raise ValueError('cycle')
        seen.add(i)
        o = tree + i * 22
        no, fl = struct.unpack_from('>IH', b, o)
        nm = rel.get(no)
        if i and nm is None:
            raise ValueError('bad name')
        p = prefix + '/' + nm if i else ''
        if fl & 2:
            cnt, first = struct.unpack_from('>II', b, o + 6)
            for c in range(first, first + cnt):
                walk(c, p)
        else:
            doff = struct.unpack_from('>I', b, o + 10)[0]
            files.append((p, fl, doff))

    walk(0, '')
    return files


def file_ok(b, base, f):
    p, fl, d = f
    o = base + d
    if o < 0 or o + 9 > len(b):
        return False
    sz = struct.unpack_from('>I', b, o)[0]
    if sz == 0 or sz > 60_000_000 or o + 4 + sz > len(b):
        return False
    if fl & 4:
        return b[o + 4:o + 8] == b'\x28\xb5\x2f\xfd'
    if fl & 1:
        return b[o + 8] == 0x78
    return True


def find_data(b, files, names_base):
    # Anchor on a compressed file (strong magic), else fall back to the
    # "data ends right before names" heuristic.
    comp = [f for f in files if f[1] & 5]
    checks = sorted(files, key=lambda f: f[2])
    if comp:
        a = comp[0]
        magic = b'\x28\xb5\x2f\xfd' if a[1] & 4 else None
        lo = max(0, names_base - 40_000_000)
        hi = min(len(b), names_base + 40_000_000)
        if magic:
            it = (m.start() - 4 - a[2] for m in re.finditer(re.escape(magic), b[lo:hi]))
            it = (x + lo for x in it)
        else:
            it = (m.start() - 8 - a[2] + lo for m in re.finditer(rb'\x78[\x01\x5e\x9c\xda]', b[lo:hi]))
        for cb in it:
            if all(file_ok(b, cb, f) for f in checks[:12]) and all(file_ok(b, cb, f) for f in checks[-4:]):
                return cb
    # plain only: data ends at names_base - pad
    last = checks[-1]
    for pad in range(0, 64):
        end = names_base - pad
        q = end - 4
        while q > end - 30_000_000 and q > 0:
            sz = struct.unpack_from('>I', b, q)[0]
            if q + 4 + sz == end:
                cb = q - last[2]
                if all(file_ok(b, cb, f) for f in checks[:12]):
                    return cb
            q -= 1
            if end - q > 2_000_000:
                break
    return None


def main(path, out):
    b = open(path, 'rb').read()
    names = find_names(b)
    print('name entries', len(names), file=sys.stderr)
    blobs = [bl for bl in group(names) if len(bl) >= 2]
    print('name blobs', len(blobs), file=sys.stderr)
    dctx = zstandard.ZstdDecompressor()
    total = 0
    for bl in blobs:
        base = bl[0]
        rel = {o - base: names[o] for o in bl}
        files = None
        for tree in find_tree(b, rel):
            try:
                cand = walk_tree(b, tree, rel)
            except (ValueError, struct.error, RecursionError):
                continue
            # the tree must use the last name of this blob
            lastname = rel[max(rel)]
            if any(f[0].endswith('/' + lastname) or ('/' + lastname + '/') in f[0] for f in cand):
                files = cand
                break
        if files is None:
            print('no tree:', list(rel.values())[:3], file=sys.stderr)
            continue
        files = [f for f in files if f[0]]
        if not files:
            continue
        dbase = find_data(b, files, base)
        if dbase is None:
            print('no data:', files[0][0], len(files), file=sys.stderr)
            continue
        for p, fl, d in files:
            sz = struct.unpack_from('>I', b, dbase + d)[0]
            raw = b[dbase + d + 4:dbase + d + 4 + sz]
            try:
                if fl & 4:
                    data = dctx.decompress(raw, max_output_size=60_000_000)
                elif fl & 1:
                    data = zlib.decompress(raw[4:])
                else:
                    data = raw
            except Exception as ex:
                print('decomp fail', p, ex, file=sys.stderr)
                continue
            dst = os.path.join(out, p.lstrip('/'))
            if os.path.isdir(dst):
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as fh:
                fh.write(data)
            total += 1
    print('files written', total, file=sys.stderr)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
