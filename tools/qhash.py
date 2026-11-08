"""qmldiff hash helpers.

  qhash.py unhash <hashtab> <file.qmd>     print plain-text version
  qhash.py hash <name> [...]               print hashes
  qhash.py check <hashtab> <name> [...]    hash + whether known in hashtab
"""
import re, struct, sys


def h(s):
    x = 5481
    for ch in s:
        x = ((x << 5) + x + (ord(ch) & 0xff)) & 0xffffffffffffffff
    return x


def load(path):
    t = {}
    b = open(path, 'rb').read()
    i = 0
    while i + 12 <= len(b):
        hv, ln = struct.unpack_from('>QI', b, i)
        t[hv] = b[i + 12:i + 12 + ln].decode('utf-8', 'replace')
        i += 12 + ln
    return t


def unhash(t, text):
    def rep(m):
        q, num = m.group(1), int(m.group(2))
        v = t.get(num, f'<?{num}?>')
        return f'"{v}"' if q else v
    text = re.sub(r'~&("?)(\d+)&~', rep, text)
    text = re.sub(r'\[\[(\d+)\]\]', lambda m: t.get(int(m.group(1)), f'<?{m.group(1)}?>'), text)
    text = re.sub(r"\[\[(\d+)\.(\d+)\]\]", lambda m: t.get(int(m.group(1)), m.group(1)) + "." + t.get(int(m.group(2)), m.group(2)), text)
    return text


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'unhash':
        print(unhash(load(sys.argv[2]), open(sys.argv[3]).read()))
    elif cmd == 'hash':
        for n in sys.argv[2:]:
            print(h(n), n)
    elif cmd == 'check':
        t = load(sys.argv[2])
        for n in sys.argv[3:]:
            print(h(n), n, 'OK' if h(n) in t else 'MISSING')
