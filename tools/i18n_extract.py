#!/usr/bin/env python3
"""Extract Russian UI strings from sources/playbooks and report which are missing in lang/en.txt.
Usage: python3 tools/i18n_extract.py [--missing] [--dump]   (run from the project root)
Dictionary format (lang/en.txt): `русский текст ==> English text`; {} is a wildcard for numbers/names; \\n \\r \\t \\\\ are escapes.
"""
import re, sys, glob, os

CYR = re.compile(r'[\u0400-\u04FF]')
LIT = re.compile(r'(L?)"((?:[^"\\\n]|\\.)*)"')
SPEC = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:ll|l|z|h|I64)?[sdiuxXcfgeEGp])')

def runtime(s):          # C++ escapes -> runtime characters
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            out.append({'n': '\n', 'r': '\r', 't': '\t', '\\': '\\', '"': '"', "'": "'"}.get(n, '\\' + n)); i += 2
        else:
            out.append(c); i += 1
    return ''.join(out)

def encode(s):           # runtime -> dictionary file escapes
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\r', '\\r').replace('\t', '\\t')

def fmt_to_tmpl(s):
    return SPEC.sub(lambda m: '%' if m.group(0) == '%%' else '{}', s)

def literals_from_cpp(text):
    text = re.sub(r'"\s*\n\s*"', '', text)          # implicit concatenation across lines
    keys = set()
    for m in LIT.finditer(text):
        raw = m.group(2)
        if not CYR.search(raw):
            continue
        s = fmt_to_tmpl(runtime(raw))
        before = text[max(0, m.start() - 12):m.start()].rstrip()
        after = text[m.end():m.end() + 12].lstrip()
        # a literal glued to run-time values with '+' is never shown alone: register only the template form seen at the output boundary
        pre = before.endswith('+') and not before.endswith('++')
        post = after.startswith('+') and not after.startswith('++')
        if pre and post: keys.add('{}' + s + '{}')
        elif post: keys.add(s + '{}')
        elif pre: keys.add('{}' + s)
        else: keys.add(s)
    return keys

def literals_from_wrp(text):
    keys = set()
    for line in text.splitlines():
        if '=' in line and not line.lstrip().startswith('#'):
            v = line.split('=', 1)[1].strip()
            if CYR.search(v):
                keys.add(fmt_to_tmpl(v))
        elif CYR.search(line) and not line.lstrip().startswith('#') and line[:1] in ' \t':
            keys.add(line.strip())
    return keys

def collect():
    keys = set()
    for p in glob.glob('src/**/*.cpp', recursive=True) + glob.glob('src/**/*.h', recursive=True):
        if p.endswith('i18n.h') or p.endswith('textutil.h'):
            continue
        keys |= literals_from_cpp(open(p, encoding='utf-8').read())
    for p in glob.glob('Playbooks/*.wrp'):
        keys |= literals_from_wrp(open(p, encoding='utf-8').read())
    return keys

def load_dict(path='lang/en.txt'):
    d = {}
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            line = line.rstrip('\n')
            if not line or line.startswith('#') or ' ==> ' not in line:
                continue
            k, v = line.split(' ==> ', 1)
            d[k] = v
    return d

if __name__ == '__main__':
    keys = sorted(collect(), key=lambda k: (len(k), k))
    d = load_dict()
    missing = [k for k in keys if encode(k) not in d]
    if '--numbered' in sys.argv:
        for i, k in enumerate(keys, 1): print(f'{i}\t{encode(k)}')
    elif '--dump' in sys.argv:
        for k in keys: print(encode(k))
    elif '--missing' in sys.argv:
        for k in missing: print(encode(k))
    print(f'# strings: {len(keys)}, translated: {len(keys) - len(missing)}, missing: {len(missing)}', file=sys.stderr)
