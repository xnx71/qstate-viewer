#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
contract_layouts_b.py -- reference layout calculator for Qubic contract state structs
(contract indices 16..30, TestExampleA-D, EmptyTemplate) for two core versions:
    v1.303.2 (EPOCH 229, matches the sample state files)  and  HEAD (v1.306.0, EPOCH 233).

It is DATA DRIVEN: the only inputs are
  (1) EXCERPTS  - verbatim line ranges copied from the Qubic core headers (see the blob at the end of
                  this file; every range carries its file name and 1-based line numbers), and
  (2) CONTRACTS - the index -> header/struct/state-type table copied from
                  src/contract_core/contract_def.h (contractDescriptions[]).
A small tolerant C++-subset front end (tokenizer, object-like #define, declaration parser that skips
member functions, constant-expression evaluator, template instantiation incl. default arguments and
(partial) specializations, typedef / using, enums, unions, C arrays, base classes) turns the excerpts
into types, and a layout engine applies the x86-64 "natural alignment" rules:

  * primitive: size == alignment (1, 2, 4, 8); bool/char/bit = 1
  * m256i / id: union of integer arrays -> size 32, ALIGNMENT 8 (not 32!);  uint128_t: 2 x uint64 -> 16 / 8
  * array T[N]: size N * sizeof(T), alignment of T
  * struct: members in declaration order, each at the next multiple of its alignment; alignment of the
    struct = max member (and base) alignment; size rounded up to the struct alignment; empty struct = 1
  * union: every member at offset 0; size = max member size rounded up to max alignment
  * base class: laid out first at offset 0 with its full sizeof() (MSVC rule; identical to the Itanium
    rule whenever the base has no tail padding -- a warning is recorded if a base has tail padding);
    empty bases take no space
  * enum: size of the underlying type (default int = 4)
  * static members, typedefs, nested type definitions and member functions take no space

Modes (combine freely):
  --json OUT            write the JSON reference (fields with name/type/offset/size, total size) for both versions
  --markdown OUT        write per-contract Markdown tables (used for the research report)
  --verify-src          check every embedded excerpt line-by-line against the core checkouts and check the
                        CONTRACTS table against contract_def.h (roots: --root-old / --root-head)
  --state-dir DIR       compare computed sizes with the files DIR/contractNNNN.229 (v1.303.2 definitions)
  --emit-oracle V OUT   write a C++ probe (compiled with g++ against the real headers, see --help-oracle)
  --check-oracle V FILE compare the probe output with the computed layouts
  --regen-blob          re-extract all excerpt ranges from the core checkouts and print the blob
Exit code is non-zero if any check fails.
"""
import argparse
import json
import os
import re
import sys

DEFAULT_ROOTS = {
    'v1.303.2': '/tmp/claude-1000/-home-yeti-devwork-space/51418eaa-0459-4cf2-bc49-43ecdf26d5ce/scratchpad/core-v1.303.2/src',
    'HEAD': '/home/yeti/devwork/space/core/src',
}
VERSION_INFO = {
    'v1.303.2': {'coreVersion': '1.303.2', 'epoch': 229},
    'HEAD': {'coreVersion': '1.306.0', 'epoch': 233},
}
VERSIONS = ['v1.303.2', 'HEAD']

# ----------------------------------------------------------------------------------------------------------
# CONTRACTS: copied from src/contract_core/contract_def.h (checked by --verify-src).
#   index: contract index per version (None = contract does not exist in that version)
#   state: the type whose sizeof() is used as stateSize in contractDescriptions[]
#   struct: the C++ contract struct; <struct>::StateData is what the contract code reads/writes
#   test: only present when INCLUDE_CONTRACT_TEST_EXAMPLES is defined (test builds); index = last index + n
# ----------------------------------------------------------------------------------------------------------
CONTRACTS = [
    dict(struct='RL', define='RL', asset='RL', header='contracts/RandomLottery.h', constructionEpoch=182,
         state='RL::StateData', index={'v1.303.2': 16, 'HEAD': 16}),
    dict(struct='QBOND', define='QBOND', asset='QBOND', header='contracts/QBond.h', constructionEpoch=182,
         state='QBOND::StateData', index={'v1.303.2': 17, 'HEAD': 17}),
    dict(struct='QIP', define='QIP', asset='QIP', header='contracts/QIP.h', constructionEpoch=189,
         state='QIP::StateData', index={'v1.303.2': 18, 'HEAD': 18}),
    dict(struct='QRAFFLE', define='QRAFFLE', asset='QRAFFLE', header='contracts/QRaffle.h', constructionEpoch=192,
         state='QRAFFLE::StateData', index={'v1.303.2': 19, 'HEAD': 19}, extra=['QRAFFLE::OldStateData']),
    dict(struct='QRWA', define='QRWA', asset='QRWA', header='contracts/qRWA.h', constructionEpoch=197,
         state='QRWA::StateData', index={'v1.303.2': 20, 'HEAD': 20}),
    dict(struct='QRP', define='QRP', asset='QRP', header='contracts/QReservePool.h', constructionEpoch=199,
         state='IPO', index={'v1.303.2': 21, 'HEAD': 21}),
    dict(struct='QTF', define='QTF', asset='QTF', header='contracts/QThirtyFour.h', constructionEpoch=199,
         state='QTF::StateData', index={'v1.303.2': 22, 'HEAD': 22}),
    dict(struct='QDUEL', define='QDUEL', asset='QDUEL', header='contracts/QDuel.h', constructionEpoch=199,
         state='QDUEL::StateData', index={'v1.303.2': 23, 'HEAD': 23}),
    dict(struct='PULSE', define='PULSE', asset='PULSE', header='contracts/Pulse.h', constructionEpoch=204,
         state='PULSE::StateData', index={'v1.303.2': 24, 'HEAD': 24}),
    dict(struct='VOTTUNBRIDGE', define='VOTTUNBRIDGE', asset='VOTTUN', header='contracts/VottunBridge.h',
         constructionEpoch=206, state='VOTTUNBRIDGE::StateData', index={'v1.303.2': 25, 'HEAD': 25}),
    dict(struct='QUSINO', define='QUSINO', asset='QUSINO', header='contracts/Qusino.h', constructionEpoch=208,
         state='QUSINO::StateData', index={'v1.303.2': 26, 'HEAD': 26}),
    dict(struct='ESCROW', define='ESCROW', asset='ESCROW', header='contracts/Escrow.h', constructionEpoch=210,
         state='ESCROW::StateData', index={'v1.303.2': 27, 'HEAD': 27}),
    dict(struct='WOLFPACK', define='WOLFPACK', asset='GGWP', header='contracts/GGWP.h', constructionEpoch=218,
         state='WOLFPACK::StateData', index={'v1.303.2': 28, 'HEAD': 28}),
    dict(struct='QPAYHUB', define='QPAYHUB', asset='QPAYHUB', header='contracts/QPayhub.h', constructionEpoch=231,
         state='QPAYHUB::StateData', index={'v1.303.2': None, 'HEAD': 29}),
    dict(struct='QTREAT', define='QTREAT', asset='QTREAT', header='contracts/QTREAT.h', constructionEpoch=233,
         state='QTREAT::StateData', index={'v1.303.2': None, 'HEAD': 30}),
    dict(struct='TESTEXA', define='TESTEXA', asset='TESTEXA', header='contracts/TestExampleA.h', constructionEpoch=138,
         state='TESTEXA::StateData', index={'v1.303.2': 29, 'HEAD': 31}, test=True),
    dict(struct='TESTEXB', define='TESTEXB', asset='TESTEXB', header='contracts/TestExampleB.h', constructionEpoch=138,
         state='TESTEXB::StateData', index={'v1.303.2': 30, 'HEAD': 32}, test=True),
    dict(struct='TESTEXC', define='TESTEXC', asset='TESTEXC', header='contracts/TestExampleC.h', constructionEpoch=138,
         state='IPO', index={'v1.303.2': 31, 'HEAD': 33}, test=True),
    dict(struct='TESTEXD', define='TESTEXD', asset='TESTEXD', header='contracts/TestExampleD.h', constructionEpoch=155,
         state='IPO', index={'v1.303.2': 32, 'HEAD': 34}, test=True),
    # EmptyTemplate.h is a template for new contracts; it is not referenced by contract_def.h (no index).
    dict(struct='CNAME', define=None, asset=None, header='contracts/EmptyTemplate.h', constructionEpoch=None,
         state='CNAME::StateData', index={'v1.303.2': None, 'HEAD': None}, template=True),
]

# Built-in types that are not parsed from excerpts.
#   m256i   : src/platform/m256.h:9   union of int8/16/32/64 arrays + structs of them  -> 32 bytes, align 8
#   uint128_t: src/platform/uint128.h:26  class { uint64_t low; uint64_t high; }        -> 16 bytes, align 8
BUILTIN_PRIMS = {
    # canonical name: (size, align, signed, C++ spellings)
    'bool': (1, 1, False, ['bool']),
    'char': (1, 1, True, ['char']),
    'sint8': (1, 1, True, ['signed char', 'int8_t']),
    'uint8': (1, 1, False, ['unsigned char', 'uint8_t']),
    'sint16': (2, 2, True, ['short', 'signed short', 'short int', 'signed short int', 'int16_t']),
    'uint16': (2, 2, False, ['unsigned short', 'unsigned short int', 'uint16_t']),
    'sint32': (4, 4, True, ['int', 'signed', 'signed int', 'int32_t']),
    'uint32': (4, 4, False, ['unsigned', 'unsigned int', 'uint32_t']),
    'sint64': (8, 8, True, ['long long', 'signed long long', 'long long int', 'signed long long int', 'int64_t']),
    'uint64': (8, 8, False, ['unsigned long long', 'unsigned long long int', 'uint64_t']),
    'id': (32, 8, False, ['m256i']),
    'uint128': (16, 8, False, ['uint128_t']),
}
PRIM_KEYWORDS = {'signed', 'unsigned', 'char', 'short', 'int', 'long', 'bool'}


class LayoutError(Exception):
    pass


# ==========================================================================================================
# 1. Excerpt blob handling
# ==========================================================================================================
class Section:
    def __init__(self, path, versions):
        self.path = path
        self.versions = versions
        self.ranges = []   # [(first, last, [lines])]

    def text_and_linemap(self):
        """Concatenate all ranges; return (text, linemap) with linemap[i] = header line of text line i."""
        out, linemap = [], []
        for first, last, lines in self.ranges:
            for k, ln in enumerate(lines):
                out.append(ln)
                linemap.append(first + k)
        return '\n'.join(out) + '\n', linemap


def parse_blob(blob):
    sections, cur, rng = [], None, None
    for raw in blob.split('\n'):
        if raw.startswith('#@@ '):
            m = re.match(r'#@@ file=(\S+) versions=(\S+)', raw)
            cur = Section(m.group(1), m.group(2).split(','))
            sections.append(cur)
            rng = None
        elif raw.startswith('#@ '):
            m = re.match(r'#@ (\d+)-(\d+)', raw)
            rng = (int(m.group(1)), int(m.group(2)), [])
            cur.ranges.append(rng)
        elif rng is not None:
            rng[2].append(raw)
    for s in sections:
        for first, last, lines in s.ranges:
            while len(lines) > last - first + 1 and lines[-1] == '':
                lines.pop()
            if len(lines) != last - first + 1:
                raise LayoutError('blob range %s:%d-%d has %d lines' % (s.path, first, last, len(lines)))
    return sections


def read_header_lines(root, path):
    with open(os.path.join(root, path), 'rb') as f:
        return [ln.rstrip('\r').rstrip() for ln in f.read().decode('utf-8').split('\n')]


# ==========================================================================================================
# 2. Tokenizer (comments stripped, object-like #define recorded, all other # lines dropped)
# ==========================================================================================================
TOKEN_RE = re.compile(r'''
    (?P<ws>[ \t\r\f\v]+)
  | (?P<nl>\n)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<num>(?:0[xX][0-9a-fA-F']+|0[bB][01']+|[0-9][0-9']*)(?:ui64|i64|[uUlL])*)
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<str>"(?:\\.|[^"\\\n])*")
  | (?P<chr>'(?:\\.|[^'\\\n])+')
  | (?P<punct>::|<<=|>>=|<<|<=|>=|==|!=|&&|\|\||\+\+|--|->|\+=|-=|\*=|/=|%=|&=|\|=|\^=|\.\.\.|[{}()\[\];,<>=+\-*/%&|^~!?:.\#\\])
''', re.X | re.S)


class Tok:
    __slots__ = ('k', 's', 'path', 'line', 'a', 'b', 'src')

    def __init__(self, k, s, path, line, a, b, src):
        self.k, self.s, self.path, self.line, self.a, self.b, self.src = k, s, path, line, a, b, src

    def __repr__(self):
        return '%s@%s:%d' % (self.s, self.path, self.line)


def tokenize(text, path, linemap, macros):
    """Returns the token list of one section. `macros` (name -> [Tok]) is updated by #define lines."""
    toks = []
    pos, n, tline = 0, len(text), 0
    at_line_start = True
    while pos < n:
        m = TOKEN_RE.match(text, pos)
        if not m:
            raise LayoutError('cannot tokenize %s near %r' % (path, text[pos:pos + 30]))
        kind = m.lastgroup
        s = m.group(kind)
        if kind == 'nl':
            tline += 1
            at_line_start = True
        elif kind in ('ws', 'lc'):
            pass
        elif kind == 'bc':
            tline += s.count('\n')
        elif kind == 'punct' and s == '#' and at_line_start:
            # preprocessor directive: consume the logical line (with \ continuations)
            end = pos
            while True:
                e = text.find('\n', end)
                if e < 0:
                    e = n
                if text[end:e].rstrip().endswith('\\'):
                    end = e + 1
                    continue
                break
            directive = text[pos:e]
            dm = re.match(r'#\s*define\s+([A-Za-z_]\w*)(?!\()\s*(.*)$', directive, re.S)
            if dm:
                body = dm.group(2).replace('\\\n', ' ')
                macros[dm.group(1)] = [t for t in tokenize(body + '\n', path, [linemap[tline]] * (body.count('\n') + 2), {})]
            tline += text[pos:e].count('\n')
            pos = e
            continue
        else:
            if kind == 'id' and s in macros:
                for mt in macros[s]:
                    toks.append(Tok(mt.k, mt.s, path, linemap[tline], m.start(), m.end(), text))
            else:
                toks.append(Tok(kind, s, path, linemap[tline], m.start(), m.end(), text))
            at_line_start = False
        pos = m.end()
    return toks


def tok_text(toks):
    """Verbatim source text covered by a token run (whitespace collapsed)."""
    if not toks:
        return ''
    return re.sub(r'\s+', ' ', toks[0].src[toks[0].a:toks[-1].b]).strip()


def parse_int_literal(s):
    s = s.replace("'", '')
    s = re.sub(r'(ui64|i64|[uUlL]+)$', '', s)
    if s[:2] in ('0x', '0X'):
        return int(s, 16)
    if s[:2] in ('0b', '0B'):
        return int(s[2:], 2)
    if len(s) > 1 and s[0] == '0':
        return int(s, 8)
    return int(s)


# ==========================================================================================================
# 3. Declarations (AST)
# ==========================================================================================================
class Scope:
    def __init__(self, kind, name, parent, owner=None):
        self.kind, self.name, self.parent, self.owner = kind, name, parent, owner
        self.names = {}
        self.usings = []


class TBuiltin:
    def __init__(self, name):
        self.name = name


class TName:
    """Possibly qualified name; parts = [(identifier, [arg token lists] | None)]."""

    def __init__(self, parts, global_q=False):
        self.parts, self.global_q = parts, global_q


class TDecl:
    """Direct reference to a struct defined in place (struct X {...} member;)."""

    def __init__(self, decl):
        self.decl = decl


class StructDecl:
    def __init__(self, kind, name, scope_parent, tparams, spec_args, tok):
        self.kind = kind                # 'struct' | 'class' | 'union'
        self.name = name                # None for anonymous
        self.tparams = tparams          # None | [TParam]
        self.spec_args = spec_args      # None | [token list]  (explicit / partial specialization)
        self.specs = []                 # specializations of a primary template
        self.bases = []                 # [type expr]
        self.members = []               # [Field] in declaration order
        self.scope = Scope('struct', name, scope_parent, self)
        self.path, self.line = tok.path, tok.line
        self.notes = set()              # unusual constructs seen inside the body


class TParam:
    def __init__(self, kind, name, type_toks, default_toks):
        self.kind, self.name, self.type_toks, self.default_toks = kind, name, type_toks, default_toks


class Field:
    def __init__(self, type_expr, type_text, name, dims, tok, ptr=0, bits=None, init=False):
        self.type_expr, self.type_text, self.name, self.dims = type_expr, type_text, name, dims
        self.path, self.line, self.ptr, self.bits, self.init = tok.path, tok.line, ptr, bits, init


class TypedefDecl:
    def __init__(self, name, type_expr, tok):
        self.name, self.type_expr, self.path, self.line = name, type_expr, tok.path, tok.line


class ConstDecl:
    def __init__(self, name, type_expr, expr_toks, tok):
        self.name, self.type_expr, self.expr_toks, self.path, self.line = name, type_expr, expr_toks, tok.path, tok.line
        self.cache = {}


class EnumDecl:
    def __init__(self, name, underlying, scoped, scope_parent, tok):
        self.name, self.underlying, self.scoped = name, underlying, scoped
        self.scope = Scope('enum', name, scope_parent, self)
        self.enumerators = []           # [(name, expr tokens | None)]
        self.path, self.line = tok.path, tok.line
        self.values = None


class EnumeratorDecl:
    def __init__(self, enum, name):
        self.enum, self.name = enum, name


# ==========================================================================================================
# 4. Parser (tolerant: member functions, friends, static_asserts, access specifiers are skipped)
# ==========================================================================================================
OPEN = {'(': ')', '{': '}', '[': ']'}


class Parser:
    def __init__(self, toks):
        self.t = toks
        self.i = 0

    # -- token helpers --
    def peek(self, k=0):
        j = self.i + k
        return self.t[j] if j < len(self.t) else Tok('eof', '<eof>', '?', 0, 0, 0, '')

    def next(self):
        t = self.peek()
        self.i += 1
        return t

    def accept(self, s):
        if self.peek().s == s:
            self.i += 1
            return True
        return False

    def expect(self, s):
        t = self.next()
        if t.s != s:
            raise LayoutError('expected %r but found %r' % (s, t))
        return t

    def skip_balanced(self, open_s):
        close_s = OPEN[open_s]
        self.expect(open_s)
        depth = 1
        while depth:
            t = self.next()
            if t.k == 'eof':
                raise LayoutError('unbalanced %s' % open_s)
            if t.s == open_s:
                depth += 1
            elif t.s == close_s:
                depth -= 1

    def slice_angle_args(self):
        """At '<': return list of token lists (one per argument) and consume through the matching '>'."""
        self.expect('<')
        args, cur, depth, pdepth = [], [], 1, 0
        while True:
            t = self.next()
            if t.k == 'eof':
                raise LayoutError('unbalanced <')
            if t.s in OPEN:
                pdepth += 1
            elif t.s in (')', '}', ']'):
                pdepth -= 1
            elif pdepth == 0:
                if t.s == '<':
                    depth += 1
                elif t.s == '>':
                    depth -= 1
                    if depth == 0:
                        break
                elif t.s == ',' and depth == 1:
                    args.append(cur)
                    cur = []
                    continue
            cur.append(t)
        if cur or args:
            args.append(cur)
        return args

    # -- types --
    def parse_type(self):
        """Parse a type-specifier (no declarator). Returns (type expr, [tokens])."""
        start = self.i
        while self.peek().s in ('const', 'volatile', 'typename', 'mutable', 'struct', 'class', 'union', 'enum') \
                and not (self.peek().s in ('struct', 'class', 'union', 'enum') and self.peek(1).s == '{'):
            self.i += 1
        if self.peek().s in PRIM_KEYWORDS:
            words = []
            while self.peek().s in PRIM_KEYWORDS:
                words.append(self.next().s)
            te = TBuiltin(' '.join(words))
        else:
            global_q = self.accept('::')
            parts = []
            while True:
                t = self.next()
                if t.k != 'id':
                    raise LayoutError('type name expected, found %r' % t)
                args = None
                if self.peek().s == '<':
                    args = self.slice_angle_args()
                parts.append((t.s, args))
                if self.peek().s == '::' and self.peek(1).k == 'id':
                    self.i += 1
                    continue
                break
            te = TName(parts, global_q)
        while self.peek().s in ('const', 'volatile'):
            self.i += 1
        return te, self.t[start:self.i]

    # -- statements --
    def classify(self):
        """Look ahead: 'func' if a '(' (or 'operator') comes before ';' '=' '{' '[' ':' at angle depth 0."""
        j, depth, pdepth = self.i, 0, 0
        while True:
            t = self.t[j] if j < len(self.t) else None
            if t is None:
                raise LayoutError('unexpected end of excerpt')
            if t.s == 'operator':
                return 'func'
            if depth > 0:
                if t.s in OPEN:
                    pdepth += 1
                elif t.s in (')', '}', ']'):
                    pdepth -= 1
                elif pdepth == 0 and t.s == '<':
                    depth += 1
                elif pdepth == 0 and t.s == '>':
                    depth -= 1
            else:
                if t.s == '<':
                    depth += 1
                elif t.s == '(':
                    return 'func'
                elif t.s in (';', '=', '{', '[', ':'):
                    return 'var'
            j += 1

    def skip_function(self):
        while True:
            t = self.peek()
            if t.s == 'operator':
                self.i += 1
                if self.peek().s == '(' and self.peek(1).s == ')':
                    self.i += 2
                while self.peek().s != '(':
                    self.i += 1
                break
            if t.s == '(':
                break
            if t.s == '<':
                self.slice_angle_args()
                continue
            self.i += 1
        self.skip_balanced('(')
        while True:
            t = self.peek()
            if t.s == ';':
                self.i += 1
                return
            if t.s == '=':
                while self.peek().s != ';':
                    self.i += 1
                self.i += 1
                return
            if t.s == ':':
                self.i += 1
                while True:
                    while self.peek().s not in ('(', '{'):
                        self.i += 1
                    self.skip_balanced(self.peek().s)
                    if self.accept(','):
                        continue
                    break
                continue
            if t.s == '{':
                self.skip_balanced('{')
                self.accept(';')
                return
            if t.s == '(':
                self.skip_balanced('(')
                continue
            self.i += 1

    def skip_statement(self):
        """Skip to ';' (or over a braced body) at bracket depth 0."""
        while True:
            t = self.peek()
            if t.s == ';':
                self.i += 1
                return
            if t.s in OPEN:
                self.skip_balanced(t.s)
                if t.s == '{':
                    self.accept(';')
                    return
                continue
            if t.k == 'eof':
                raise LayoutError('unexpected end in statement')
            self.i += 1

    def parse_template_head(self):
        self.expect('template')
        params = []
        for a in self.slice_angle_args():
            if not a:
                continue
            default = None
            for k, t in enumerate(a):
                if t.s == '=':
                    default = a[k + 1:]
                    a = a[:k]
                    break
            if a[0].s in ('typename', 'class'):
                params.append(TParam('type', a[1].s if len(a) > 1 else None, None, default))
            else:
                params.append(TParam('value', a[-1].s, a[:-1], default))
        return params

    def parse_scope_body(self, scope, owner=None, until='}'):
        while True:
            t = self.peek()
            if t.k == 'eof':
                if until is None:
                    return
                raise LayoutError('unexpected end of excerpt in %s' % scope.name)
            if until and t.s == until:
                self.i += 1
                return
            self.parse_decl(scope, owner)

    def parse_decl(self, scope, owner):
        t = self.peek()
        s = t.s
        if s == ';':
            self.i += 1
            return
        if s in ('public', 'private', 'protected') and self.peek(1).s == ':':
            self.i += 2
            if owner is not None:
                owner.notes.add('access specifier')
            return
        if s == 'namespace':
            self.i += 1
            name = self.next().s
            ns = scope.names.get(name)
            if ns is None:
                ns = Scope('namespace', name, scope)
                scope.names[name] = ns
            self.expect('{')
            self.parse_scope_body(ns)
            return
        if s == 'using':
            if self.peek(1).s == 'namespace':
                self.i += 2
                te, _ = self.parse_type()
                scope.usings.append(te)
                self.expect(';')
                return
            if self.peek(2).s == '=':
                self.i += 1
                name = self.next()
                self.expect('=')
                te, _ = self.parse_type()
                self.expect(';')
                scope.names[name.s] = TypedefDecl(name.s, te, name)
                if owner is not None:
                    owner.notes.add('using alias')
                return
            self.skip_statement()
            return
        if s == 'typedef':
            self.i += 1
            if self.classify() == 'func':
                self.skip_statement()
                return
            te, _ = self.parse_type()
            name = self.next()
            self.expect(';')
            scope.names[name.s] = TypedefDecl(name.s, te, name)
            if owner is not None:
                owner.notes.add('typedef')
            return
        if s == 'static_assert':
            self.skip_statement()
            if owner is not None:
                owner.notes.add('static_assert')
            return
        if s == 'friend':
            self.skip_statement()
            if owner is not None:
                owner.notes.add('friend declaration')
            return
        tparams = None
        if s == 'template':
            tparams = self.parse_template_head()
            t = self.peek()
            s = t.s
            if s == 'friend':
                self.skip_statement()
                return
        if s in ('struct', 'class', 'union') and self.is_struct_definition_or_fwd():
            self.parse_struct(scope, owner, tparams)
            return
        if s == 'enum':
            self.parse_enum(scope, owner)
            return
        # specifiers
        j = self.i
        specs = set()
        while self.t[j].s in ('static', 'inline', 'constexpr', 'const', 'virtual', 'explicit', 'mutable', 'volatile'):
            specs.add(self.t[j].s)
            j += 1
        if self.classify() == 'func':
            if owner is not None:
                owner.notes.add('member function')
            self.skip_function()
            return
        self.i = j
        is_const = 'constexpr' in specs or ('const' in specs and scope.kind != 'struct')
        if scope.kind == 'struct' and 'static' not in specs and not is_const:
            self.parse_fields(scope, owner)
            return
        if scope.kind == 'struct' and 'static' not in specs and is_const:
            # non-static const member with initializer: still occupies storage
            self.parse_fields(scope, owner)
            return
        # constant (namespace scope constexpr / static constexpr member) or static member without storage
        te, _ = self.parse_type()
        name = self.next()
        if self.accept('='):
            expr = []
            while self.peek().s != ';':
                expr.append(self.next())
            self.expect(';')
            scope.names[name.s] = ConstDecl(name.s, te, expr, name)
        else:
            self.skip_statement()
        if owner is not None:
            owner.notes.add('static member')

    def is_struct_definition_or_fwd(self):
        # struct [Name[<...>]] [: bases] {   |   struct Name ;      (otherwise: elaborated type in a declaration)
        j = self.i + 1
        if self.t[j].k == 'id':
            j += 1
            if self.t[j].s == '<':
                depth = 0
                while True:
                    if self.t[j].s == '<':
                        depth += 1
                    elif self.t[j].s == '>':
                        depth -= 1
                        if depth == 0:
                            j += 1
                            break
                    j += 1
        return self.t[j].s in ('{', ':', ';')

    def parse_struct(self, scope, owner, tparams):
        kw = self.next()
        name, spec_args = None, None
        if self.peek().k == 'id':
            name = self.next().s
            if self.peek().s == '<':
                spec_args = self.slice_angle_args()
        if self.accept(';'):
            return None                      # forward declaration
        decl = StructDecl(kw.s, name, scope, tparams, spec_args, kw)
        if self.accept(':'):
            while True:
                while self.peek().s in ('public', 'private', 'protected', 'virtual'):
                    self.i += 1
                te, _ = self.parse_type()
                decl.bases.append(te)
                if not self.accept(','):
                    break
        self.expect('{')
        if spec_args is not None:
            scope.names[name].specs.append(decl)
        elif name is not None:
            scope.names[name] = decl
        self.parse_scope_body(decl.scope, decl)
        if owner is not None:
            owner.notes.add('nested type definition' if name else 'anonymous %s' % kw.s)
        # declarators after the closing brace
        if self.peek().s == ';':
            self.i += 1
            if name is None and owner is not None:
                owner.members.append(Field(TDecl(decl), '%s { ... }' % kw.s, '', [], kw))
            return decl
        if owner is None:
            self.skip_statement()
            return decl
        self.parse_declarators(owner, TDecl(decl), '%s %s' % (kw.s, name or '{ ... }'))
        return decl

    def parse_enum(self, scope, owner):
        kw = self.next()
        scoped = False
        if self.peek().s in ('class', 'struct'):
            self.i += 1
            scoped = True
        name = None
        if self.peek().k == 'id':
            name = self.next().s
        underlying = None
        if self.accept(':'):
            underlying, _ = self.parse_type()
        if self.accept(';'):
            return
        decl = EnumDecl(name, underlying, scoped, scope, kw)
        self.expect('{')
        while not self.accept('}'):
            en = self.next()
            expr = None
            if self.accept('='):
                expr, depth = [], 0
                while True:
                    t = self.peek()
                    if depth == 0 and t.s in (',', '}'):
                        break
                    if t.s in OPEN:
                        depth += 1
                    elif t.s in (')', '}', ']'):
                        depth -= 1
                    expr.append(self.next())
            decl.enumerators.append((en.s, expr))
            (decl.scope if scoped else scope).names[en.s] = EnumeratorDecl(decl, en.s)
            self.accept(',')
        if name:
            scope.names[name] = decl
        if owner is not None:
            owner.notes.add('nested enum definition')
        if self.peek().s == ';':
            self.i += 1
        elif owner is not None:
            self.parse_declarators(owner, TDecl(decl), 'enum %s' % (name or ''))

    def parse_fields(self, scope, owner):
        te, ttoks = self.parse_type()
        self.parse_declarators(owner, te, tok_text(ttoks))

    def parse_declarators(self, owner, te, type_text):
        count = 0
        while True:
            ptr = 0
            while self.peek().s in ('*', '&', 'const'):
                if self.next().s == '*':
                    ptr += 1
            name = self.next()
            if name.k != 'id':
                raise LayoutError('declarator name expected, found %r' % name)
            dims = []
            while self.accept('['):
                expr, depth = [], 0
                while True:
                    t = self.next()
                    if t.s == '[':
                        depth += 1
                    elif t.s == ']':
                        if depth == 0:
                            break
                        depth -= 1
                    expr.append(t)
                dims.append(expr)
            bits = None
            init = False
            if self.accept(':'):
                bits = []
                while self.peek().s not in (',', ';', '='):
                    bits.append(self.next())
                owner.notes.add('bit-field')
            if self.peek().s == '=':
                init = True
                depth = 0
                while True:
                    t = self.peek()
                    if depth == 0 and t.s in (',', ';'):
                        break
                    if t.s in OPEN:
                        depth += 1
                    elif t.s in (')', '}', ']'):
                        depth -= 1
                    self.i += 1
                owner.notes.add('default member initializer')
            elif self.peek().s == '{':
                init = True
                self.skip_balanced('{')
                owner.notes.add('default member initializer')
            owner.members.append(Field(te, type_text, name.s, dims, name, ptr, bits, init))
            if ptr:
                owner.notes.add('pointer member')
            if dims:
                owner.notes.add('plain C array')
            count += 1
            if self.accept(','):
                continue
            self.expect(';')
            break
        if count > 1:
            owner.notes.add('several declarators in one statement')


# ==========================================================================================================
# 5. Resolved types + layout engine
# ==========================================================================================================
def align_up(x, a):
    return (x + a - 1) // a * a


class RType:
    size = align = 0
    cname = '?'
    kind = '?'


class RPrim(RType):
    kind = 'primitive'

    def __init__(self, cname, size, align, signed):
        self.cname, self.size, self.align, self.signed = cname, size, align, signed

    def wrap(self, v):
        if self.cname == 'bool':
            return 1 if v else 0
        bits = self.size * 8
        v &= (1 << bits) - 1
        if self.signed and v >> (bits - 1):
            v -= 1 << bits
        return v


class RPtr(RType):
    kind = 'pointer'

    def __init__(self, target):
        self.size, self.align, self.cname = 8, 8, target.cname + '*'


class RArray(RType):
    kind = 'array'

    def __init__(self, elem, count):
        self.elem, self.count = elem, count
        self.size, self.align = elem.size * count, elem.align
        # cname in C++ declarator order: T[a][b]
        base, dims = elem, [count]
        while isinstance(base, RArray):
            dims.append(base.count)
            base = base.elem
        self.cname = base.cname + ''.join('[%d]' % d for d in dims)


class REnum(RType):
    kind = 'enum'

    def __init__(self, decl, underlying, cname):
        self.decl, self.underlying, self.cname = decl, underlying, cname
        self.size, self.align = underlying.size, underlying.align


class RField:
    def __init__(self, name, rtype, offset, decl):
        self.name, self.type, self.offset, self.decl = name, rtype, offset, decl


class Lazy:
    def __init__(self, toks, kind, env):
        self.toks, self.kind, self.env = toks, kind, env


class Env:
    __slots__ = ('scope', 'inst')

    def __init__(self, scope, inst):
        self.scope, self.inst = scope, inst


class RStruct(RType):
    def __init__(self, world, decl, bindings, outer, cname, targs_text=None):
        self.world, self.decl, self.bindings, self.outer, self.cname = world, decl, bindings, outer, cname
        self.kind = 'union' if decl.kind == 'union' else 'struct'
        self._laid = False
        self._bases = None
        self.warnings = []

    def env(self):
        return Env(self.decl.scope, self)

    def bases(self):
        if self._bases is None:
            # base-specifier names are looked up in the class-head scope (enclosing scope + template parameters);
            # while doing so, the base list of this instance itself must not be consulted
            self._bases = []
            self._bases = [self.world.resolve_type(b, self.env()) for b in self.decl.bases]
        return self._bases

    def lay_out(self):
        if self._laid:
            return self
        self._laid = True
        w = self.world
        off, al, end_union = 0, 1, 0
        self.fields, self.base_fields, self.empty = [], [], True
        for b in self.bases():
            if not isinstance(b, RStruct):
                raise LayoutError('base class of %s is not a struct' % self.cname)
            b.lay_out()
            if b.empty:
                self.base_fields.append((b, 0))
                continue
            off = align_up(off, b.align)
            self.base_fields.append((b, off))
            if b.dsize != b.size:
                self.warnings.append('base %s has %d bytes of tail padding (MSVC and Itanium ABIs may place '
                                     'the first derived member differently)' % (b.cname, b.size - b.dsize))
            off += b.size
            al = max(al, b.align)
            self.empty = False
        env = self.env()
        for m in self.decl.members:
            t = w.resolve_type(m.type_expr, env)
            if m.bits is not None:
                raise LayoutError('bit-field %s::%s is not supported by this reference' % (self.cname, m.name))
            if isinstance(t, RStruct):
                t.lay_out()
            for _ in range(m.ptr):
                t = RPtr(t)
            for d in reversed(m.dims):
                t = RArray(t, w.eval(d, env))
            if self.kind == 'union':
                fo = 0
                end_union = max(end_union, t.size)
            else:
                off = align_up(off, t.align)
                fo = off
                off += t.size
            al = max(al, t.align)
            self.fields.append(RField(m.name, t, fo, m))
            self.empty = False
        if self.kind == 'union':
            off = end_union
        self.dsize = off                    # size without tail padding
        self.align = al
        self.size = align_up(off, al) if not self.empty else 1
        return self


class World:
    """All declarations of one core version + type resolution / instantiation."""

    def __init__(self, sections, version):
        self.version = version
        self.globals = Scope('global', '', None)
        self.macros = {}
        self.prims = {}
        self.spelling = {}
        for cname, (size, align, signed, spellings) in BUILTIN_PRIMS.items():
            p = RPrim(cname, size, align, signed)
            self.prims[cname] = p
            for sp in spellings:
                self.spelling[sp] = p
        for name in ('bool', 'char'):
            self.spelling[name] = self.prims[name]
        self.inst_cache = {}
        self.typedef_cache = {}
        self.enum_cache = {}
        for sec in sections:
            if version not in sec.versions:
                continue
            text, linemap = sec.text_and_linemap()
            toks = tokenize(text, sec.path, linemap, self.macros)
            p = Parser(toks)
            try:
                p.parse_scope_body(self.globals, None, until=None)
            except LayoutError as e:
                raise LayoutError('%s (while parsing %s near %r)' % (e, sec.path, p.peek()))
        # resolve using-directives of the global scope
        self.globals.usings = [self.globals.names[u.parts[0][0]] for u in self.globals.usings
                               if u.parts[0][0] in self.globals.names]
        seen = []
        for u in self.globals.usings:
            if u not in seen:
                seen.append(u)
        self.globals.usings = seen

    # ---- name lookup ----
    def found(self, d, env):
        if isinstance(d, Scope):
            return ('namespace', d)
        if isinstance(d, StructDecl):
            if d.tparams is not None:
                return ('template', d, env)
            return ('type', self.instantiate(d, {}, env.inst))
        if isinstance(d, TypedefDecl):
            key = (id(d), id(env.inst))
            if key not in self.typedef_cache:
                self.typedef_cache[key] = self.resolve_type(d.type_expr, env)
            return ('type', self.typedef_cache[key])
        if isinstance(d, ConstDecl):
            key = id(env.inst)
            if key not in d.cache:
                t = self.resolve_type(d.type_expr, env)
                v = self.eval(d.expr_toks, env)
                d.cache[key] = (t.wrap(v) if isinstance(t, RPrim) else v, t)
            return ('value',) + d.cache[key]
        if isinstance(d, EnumDecl):
            return ('type', self.enum_type(d, env))
        if isinstance(d, EnumeratorDecl):
            et = self.enum_type(d.enum, env)
            return ('value', self.enum_values(d.enum, env)[d.name], et)
        raise LayoutError('unknown declaration kind %r' % d)

    def lookup(self, env, name):
        scope, inst = env.scope, env.inst
        while scope is not None:
            if scope.kind == 'struct':
                if inst is None or inst.decl.scope is not scope:
                    raise LayoutError('internal: instance chain out of sync at %s' % name)
                if name in inst.bindings:
                    return self.bound(inst.bindings, name)
                if name == inst.decl.name:
                    return ('type', inst)
                d = scope.names.get(name)
                if d is not None:
                    return self.found(d, Env(scope, inst))
                r = self.lookup_bases(inst, name)
                if r is not None:
                    return r
                inst = inst.outer
            else:
                d = scope.names.get(name)
                if d is not None:
                    return self.found(d, Env(scope, None))
                for u in scope.usings:
                    d = u.names.get(name)
                    if d is not None:
                        return self.found(d, Env(u, None))
            scope = scope.parent
        if name in self.spelling:
            return ('type', self.spelling[name])
        if name in self.prims:
            return ('type', self.prims[name])
        return None

    def bound(self, bindings, name):
        b = bindings[name]
        if isinstance(b, Lazy):
            if b.kind == 'type':
                p = Parser(b.toks)
                te, _ = p.parse_type()
                b = self.resolve_type(te, b.env)
            else:
                b = self.eval(b.toks, b.env)
            bindings[name] = b
        if isinstance(b, RType):
            return ('type', b)
        return ('value', b[0], b[1]) if isinstance(b, tuple) else ('value', b, None)

    def lookup_bases(self, inst, name):
        for b in inst.bases():
            r = self.member(b, name)
            if r is not None:
                return r
        return None

    def member(self, container, name):
        if isinstance(container, Scope):
            d = container.names.get(name)
            return self.found(d, Env(container, None)) if d is not None else None
        if isinstance(container, RStruct):
            d = container.decl.scope.names.get(name)
            if d is not None:
                return self.found(d, Env(container.decl.scope, container))
            return self.lookup_bases(container, name)
        if isinstance(container, REnum):
            vals = self.enum_values_of(container)
            if name in vals:
                return ('value', vals[name], container)
        return None

    # ---- enums ----
    def enum_type(self, d, env):
        key = (id(d), id(env.inst))
        if key not in self.enum_cache:
            under = self.resolve_type(d.underlying, env) if d.underlying is not None else self.prims['sint32']
            owner = (env.inst.cname + '::') if env.inst is not None else self.scope_prefix(env.scope)
            et = REnum(d, under, owner + (d.name or '<anonymous enum@%d>' % d.line))
            et.env = env
            self.enum_cache[key] = et
        return self.enum_cache[key]

    def enum_values(self, d, env):
        return self.enum_values_of(self.enum_type(d, env))

    def enum_values_of(self, et):
        if not hasattr(et, 'values'):
            vals, cur = {}, 0
            et.values = vals
            inner = Env(et.decl.scope, None) if et.decl.scoped else None
            for name, expr in et.decl.enumerators:
                if expr is not None:
                    cur = self.eval(expr, et.env, partial_enum=vals)
                vals[name] = cur
                cur += 1
        return et.values

    # ---- types ----
    def scope_prefix(self, scope):
        parts = []
        while scope is not None and scope.kind != 'global':
            if scope.name and scope.name != 'QPI':
                parts.append(scope.name)
            scope = scope.parent
        return ''.join(p + '::' for p in reversed(parts))

    def resolve_type(self, te, env):
        if isinstance(te, TDecl):
            if isinstance(te.decl, EnumDecl):
                return self.enum_type(te.decl, env)
            return self.instantiate(te.decl, {}, env.inst)
        if isinstance(te, TBuiltin):
            if te.name not in self.spelling:
                raise LayoutError('unsupported builtin type %r (note: long / float / double are not handled)' % te.name)
            return self.spelling[te.name]
        cur = None
        for k, (name, args) in enumerate(te.parts):
            if k == 0:
                r = self.lookup(Env(self.globals, None) if te.global_q else env, name)
            else:
                r = self.member(cur, name)
            if r is None:
                raise LayoutError('unknown name %r in type %s' % (name, '::'.join(p[0] for p in te.parts)))
            if r[0] == 'template':
                if args is None:
                    raise LayoutError('template %s used without arguments' % name)
                cur = self.instantiate_template(r[1], args, env, r[2])
            elif r[0] == 'namespace':
                cur = r[1]
            elif r[0] == 'type':
                cur = r[1]
            else:
                raise LayoutError('%r is not a type' % name)
        if not isinstance(cur, RType):
            raise LayoutError('%r does not name a type' % '::'.join(p[0] for p in te.parts))
        return cur

    def fmt_arg(self, v):
        if isinstance(v, RType):
            return v.cname
        val, t = v
        if t is not None and t.cname == 'bool':
            return 'true' if val else 'false'
        return str(val)

    def instantiate_template(self, decl, arg_toks, use_env, def_env):
        actual = []
        tmp = {}
        for k, p in enumerate(decl.tparams):
            if k < len(arg_toks):
                if p.kind == 'type':
                    te, _ = Parser(arg_toks[k]).parse_type()
                    actual.append(self.resolve_type(te, use_env))
                else:
                    pt = self.resolve_type(Parser(p.type_toks).parse_type()[0], def_env)
                    actual.append((pt.wrap(self.eval(arg_toks[k], use_env)), pt))
            elif p.default_toks is not None:
                actual.append(None)          # default argument: bound lazily below
            else:
                raise LayoutError('too few template arguments for %s' % decl.name)
        explicit = [a for a in actual if a is not None]
        prefix = (def_env.inst.cname + '::') if def_env.inst is not None else self.scope_prefix(def_env.scope)
        cname = '%s%s<%s>' % (prefix, decl.name, ','.join(self.fmt_arg(a) for a in explicit))
        key = (id(decl), id(def_env.inst), cname)
        if key in self.inst_cache:
            return self.inst_cache[key]
        chosen, bindings = decl, None
        for spec in decl.specs:
            b = self.match_spec(spec, actual, def_env)
            if b is not None:
                chosen, bindings = spec, b
                break
        inst = RStruct(self, chosen, {}, def_env.inst, cname)
        if bindings is None:
            bindings = {}
            for p, a in zip(decl.tparams, actual):
                if a is None:
                    bindings[p.name] = Lazy(p.default_toks, p.kind, Env(chosen.scope, inst))
                else:
                    bindings[p.name] = a
        inst.bindings = bindings
        inst.template_args = explicit
        self.inst_cache[key] = inst
        return inst

    def match_spec(self, spec, actual, def_env):
        names = [p.name for p in (spec.tparams or [])]
        b = {}
        for pat, a in zip(spec.spec_args, actual):
            if len(pat) == 1 and pat[0].s in names:
                b[pat[0].s] = a
                continue
            if isinstance(a, RType):
                te, _ = Parser(pat).parse_type()
                if self.resolve_type(te, def_env) is not a:
                    return None
            else:
                if self.eval(pat, def_env) != a[0]:
                    return None
        return b

    def instantiate(self, decl, bindings, outer):
        key = (id(decl), id(outer), '')
        if key not in self.inst_cache:
            prefix = (outer.cname + '::') if outer is not None else self.scope_prefix(decl.scope.parent)
            cname = prefix + (decl.name or '<anonymous %s@%d>' % (decl.kind, decl.line))
            self.inst_cache[key] = RStruct(self, decl, dict(bindings), outer, cname)
        return self.inst_cache[key]

    def type_by_name(self, text):
        """Resolve a type written as text in the global scope, e.g. 'RL::StateData'."""
        toks = tokenize(text + '\n', '<query>', [0, 0], dict(self.macros))
        te, _ = Parser(toks).parse_type()
        t = self.resolve_type(te, Env(self.globals, None))
        if isinstance(t, RStruct):
            t.lay_out()
        return t

    # ---- constant expressions ----
    def eval(self, toks, env, partial_enum=None):
        return ExprEval(self, toks, env, partial_enum).run()


def c_div(a, b):
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


class ExprEval:
    def __init__(self, world, toks, env, partial_enum=None):
        self.w, self.t, self.env, self.i, self.partial_enum = world, toks, env, 0, partial_enum or {}

    def peek(self, k=0):
        j = self.i + k
        return self.t[j] if j < len(self.t) else None

    def ps(self, k=0):
        t = self.peek(k)
        return t.s if t is not None else None

    def run(self):
        v = self.ternary()
        if self.i != len(self.t):
            raise LayoutError('trailing tokens in expression %r' % tok_text(self.t))
        return v

    def ternary(self):
        c = self.binary(0)
        if self.ps() == '?':
            self.i += 1
            a = self.ternary()
            if self.ps() != ':':
                raise LayoutError('":" expected in %r' % tok_text(self.t))
            self.i += 1
            b = self.ternary()
            return a if c else b
        return c

    LEVELS = [['||'], ['&&'], ['|'], ['^'], ['&'], ['==', '!='], ['<', '>', '<=', '>='], ['<<', '>>'],
              ['+', '-'], ['*', '/', '%']]

    def op_at(self, level):
        s = self.ps()
        if s is None:
            return None
        if s == '>':
            nxt = self.peek(1)
            if nxt is not None and nxt.s == '>' and nxt.a == self.peek().b:
                s = '>>'
        if s in self.LEVELS[level]:
            self.i += 2 if s == '>>' else 1
            return s
        return None

    def binary(self, level):
        if level == len(self.LEVELS):
            return self.unary()
        v = self.binary(level + 1)
        while True:
            op = self.op_at(level)
            if op is None:
                return v
            r = self.binary(level + 1)
            if op == '||':
                v = 1 if (v or r) else 0
            elif op == '&&':
                v = 1 if (v and r) else 0
            elif op == '|':
                v |= r
            elif op == '^':
                v ^= r
            elif op == '&':
                v &= r
            elif op == '==':
                v = int(v == r)
            elif op == '!=':
                v = int(v != r)
            elif op == '<':
                v = int(v < r)
            elif op == '>':
                v = int(v > r)
            elif op == '<=':
                v = int(v <= r)
            elif op == '>=':
                v = int(v >= r)
            elif op == '<<':
                v <<= r
            elif op == '>>':
                v >>= r
            elif op == '+':
                v += r
            elif op == '-':
                v -= r
            elif op == '*':
                v *= r
            elif op == '/':
                v = c_div(v, r)
            elif op == '%':
                v = v - c_div(v, r) * r

    def unary(self):
        s = self.ps()
        if s == '-':
            self.i += 1
            return -self.unary()
        if s == '+':
            self.i += 1
            return self.unary()
        if s == '~':
            self.i += 1
            return ~self.unary()
        if s == '!':
            self.i += 1
            return int(not self.unary())
        return self.primary()

    def paren_tokens(self):
        """At '(' : return the tokens inside the balanced parentheses and consume them."""
        assert self.ps() == '('
        depth, j = 0, self.i
        while True:
            s = self.t[j].s
            if s == '(':
                depth += 1
            elif s == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        inner = self.t[self.i + 1:j]
        self.i = j + 1
        return inner

    def try_type(self, toks):
        try:
            p = Parser(list(toks))
            te, _ = p.parse_type()
            if p.i != len(toks):
                return None
            return self.w.resolve_type(te, self.env)
        except (LayoutError, IndexError):
            return None

    def split_args(self, toks):
        args, cur, depth = [], [], 0
        for t in toks:
            if t.s in OPEN:
                depth += 1
            elif t.s in (')', '}', ']'):
                depth -= 1
            if t.s == ',' and depth == 0:
                args.append(cur)
                cur = []
            else:
                cur.append(t)
        if cur:
            args.append(cur)
        return args

    def primary(self):
        t = self.peek()
        if t is None:
            raise LayoutError('unexpected end of expression %r' % tok_text(self.t))
        if t.k == 'num':
            self.i += 1
            return parse_int_literal(t.s)
        if t.k == 'chr':
            self.i += 1
            body = t.s[1:-1]
            return ord(body[-1]) if not body.startswith('\\') else {'n': 10, 't': 9, '0': 0, '\\': 92, "'": 39}[body[1]]
        if t.s == '(':
            inner = self.paren_tokens()
            ty = self.try_type(inner)
            if ty is not None:                       # C-style cast
                v = self.unary()
                return ty.wrap(v) if isinstance(ty, RPrim) else v
            return self.w.eval(inner, self.env, self.partial_enum)
        if t.s in ('true', 'false'):
            self.i += 1
            return int(t.s == 'true')
        if t.s == 'sizeof':
            self.i += 1
            inner = self.paren_tokens()
            ty = self.try_type(inner)
            if ty is None:
                raise LayoutError('sizeof(expression) is not supported: %r' % tok_text(inner))
            if isinstance(ty, RStruct):
                ty.lay_out()
            return ty.size
        if t.s in ('static_cast', 'div', 'mod') and self.ps(1) == '<':
            self.i += 1
            p = Parser(self.t)
            p.i = self.i
            targs = p.slice_angle_args()
            self.i = p.i
            ty = self.try_type(targs[0])
            args = [self.w.eval(a, self.env, self.partial_enum) for a in self.split_args(self.paren_tokens())]
            if t.s == 'static_cast':
                v = args[0]
            elif t.s == 'div':
                v = c_div(args[0], args[1]) if args[1] else 0
            else:
                v = (args[0] - c_div(args[0], args[1]) * args[1]) if args[1] else 0
            return ty.wrap(v) if isinstance(ty, RPrim) else v
        if t.k == 'id':
            # qualified name / functional cast / call of QPI::div, QPI::mod
            p = Parser(self.t)
            p.i = self.i
            te, used = p.parse_type()
            self.i = p.i
            if isinstance(te, TBuiltin):
                ty = self.w.resolve_type(te, self.env)
                return ty.wrap(self.w.eval(self.paren_tokens(), self.env, self.partial_enum))
            if len(te.parts) == 1 and te.parts[0][0] in self.partial_enum:
                return self.partial_enum[te.parts[0][0]]
            if len(te.parts) == 1 and te.parts[0][0] in ('div', 'mod') and self.ps() == '(':
                a, b = [self.w.eval(x, self.env, self.partial_enum) for x in self.split_args(self.paren_tokens())]
                if te.parts[0][0] == 'div':
                    return c_div(a, b) if b else 0
                return (a - c_div(a, b) * b) if b else 0
            cur = None
            for k, (name, args) in enumerate(te.parts):
                r = self.w.lookup(self.env, name) if k == 0 else self.w.member(cur, name)
                if r is None:
                    raise LayoutError('unknown name %r in expression %r' % (name, tok_text(self.t)))
                if r[0] == 'template':
                    cur = self.w.instantiate_template(r[1], args, self.env, r[2])
                elif r[0] == 'value':
                    return r[1]
                else:
                    cur = r[1]
            if isinstance(cur, RPrim) and self.ps() == '(':     # functional cast uint64(x)
                return cur.wrap(self.w.eval(self.paren_tokens(), self.env, self.partial_enum))
            raise LayoutError('%r is not a constant in %r' % (tok_text(used), tok_text(self.t)))
        raise LayoutError('unsupported token %r in expression %r' % (t, tok_text(self.t)))


# ==========================================================================================================
# 6. Reporting
# ==========================================================================================================
def collect_types(t, out):
    """All struct / union / enum / array element types reachable from t (depth first, dependency order)."""
    if isinstance(t, RArray):
        collect_types(t.elem, out)
        return
    if isinstance(t, REnum):
        out.setdefault(t.cname, t)
        return
    if not isinstance(t, RStruct):
        return
    if t.cname in out:
        return
    t.lay_out()
    for b, _ in t.base_fields:
        collect_types(b, out)
    for f in t.fields:
        collect_types(f.type, out)
    out[t.cname] = t


def declarator_text(f):
    m = f.decl
    return m.name + ''.join('[%s]' % tok_text(d) for d in m.dims)


def type_json(t):
    if isinstance(t, REnum):
        return {'kind': 'enum', 'size': t.size, 'align': t.align, 'underlying': t.underlying.cname,
                'scoped': t.decl.scoped, 'source': '%s:%d' % (t.decl.path, t.decl.line),
                'enumerators': dict(t.world_values)}
    d = {'kind': t.kind, 'size': t.size, 'align': t.align,
         'source': '%s:%d' % (t.decl.path, t.decl.line)}
    if t.base_fields:
        d['bases'] = [{'type': b.cname, 'offset': o, 'size': 0 if b.empty else b.size} for b, o in t.base_fields]
    if getattr(t, 'template_args', None):
        d['templateArgs'] = [a.cname if isinstance(a, RType) else a[0] for a in t.template_args]
    d['fields'] = [field_json(f) for f in t.fields]
    if t.decl.notes:
        d['notes'] = sorted(t.decl.notes)
    if t.warnings:
        d['warnings'] = t.warnings
    return d


def field_json(f):
    return {'name': f.name, 'type': f.decl.type_text, 'declarator': declarator_text(f),
            'resolvedType': f.type.cname, 'offset': f.offset, 'size': f.type.size, 'align': f.type.align,
            'source': '%s:%d' % (f.decl.path, f.decl.line)}


def build_version(world, version, state_dir=None):
    res = {'coreVersion': VERSION_INFO[version]['coreVersion'], 'epoch': VERSION_INFO[version]['epoch'],
           'contracts': [], 'types': {}}
    types = {}
    for c in CONTRACTS:
        idx = c['index'][version]
        if idx is None and not c.get('template'):
            continue
        sd_name = c['struct'] + '::StateData'
        sd = world.type_by_name(sd_name)
        st = world.type_by_name(c['state'])
        collect_types(sd, types)
        collect_types(st, types)
        entry = {
            'index': idx, 'assetName': c['asset'], 'struct': c['struct'], 'header': 'src/' + c['header'],
            'constructionEpoch': c['constructionEpoch'],
            'stateType': c['state'], 'stateSize': st.size,
            'stateDataType': sd.cname, 'stateDataDefinedAt': '%s:%d' % (sd.decl.path, sd.decl.line),
            'stateDataSize': sd.size, 'stateDataAlign': sd.align,
            'fields': [field_json(f) for f in sd.fields],
        }
        if c.get('test'):
            entry['onlyWithTestExamples'] = True
        if c.get('template'):
            entry['templateOnly'] = True
        if sd.decl.notes:
            entry['stateDataNotes'] = sorted(sd.decl.notes)
        for extra in c.get('extra', []):
            et = world.type_by_name(extra)
            collect_types(et, types)
            entry.setdefault('extraTypes', {})[extra] = et.size
        if state_dir and version == 'v1.303.2' and idx is not None and not c.get('test'):
            p = os.path.join(state_dir, 'contract%04d.%d' % (idx, VERSION_INFO[version]['epoch']))
            if os.path.exists(p):
                fs = os.path.getsize(p)
                entry['file'] = {'path': p, 'size': fs, 'matchesStateSize': fs == st.size,
                                 'matchesStateDataSize': fs == sd.size}
        res['contracts'].append(entry)
    for name, t in types.items():
        if isinstance(t, REnum):
            t.world_values = world.enum_values_of(t)
        res['types'][name] = type_json(t)
    return res, types


def markdown(result, version):
    out = []
    for c in result['contracts']:
        title = '%s (index %s, `%s`)' % (c['struct'], c['index'] if c['index'] is not None else 'n/a', c['header'])
        out.append('#### %s -- %s' % (version, title))
        out.append('')
        out.append('`%s`: size **%d**, align %d; contractDescriptions state type `%s` = %d bytes'
                   % (c['stateDataType'], c['stateDataSize'], c['stateDataAlign'], c['stateType'], c['stateSize']))
        out.append('')
        out.append('| offset | size | field | declared type (verbatim) | resolved type | src line |')
        out.append('|---:|---:|---|---|---|---:|')
        for f in c['fields']:
            out.append('| %d | %d | `%s` | `%s` | `%s` | %s |' % (
                f['offset'], f['size'], f['declarator'], f['type'], f['resolvedType'], f['source'].split(':')[1]))
        out.append('')
    out.append('#### %s -- nested / instantiated types' % version)
    out.append('')
    for name, t in result['types'].items():
        if t['kind'] == 'enum':
            out.append('- `%s` (enum, underlying `%s`, size %d; %s): %s' % (
                name, t['underlying'], t['size'], t['source'],
                ', '.join('%s=%d' % kv for kv in t['enumerators'].items())))
            continue
        fl = '; '.join('%s `%s` @%d (%d)' % (f['resolvedType'], f['declarator'], f['offset'], f['size'])
                       for f in t['fields'])
        base = ''
        if t.get('bases'):
            base = ' : ' + ', '.join(b['type'] for b in t['bases'])
        out.append('- `%s`%s (%s, size %d, align %d; %s): %s' % (name, base, t['kind'], t['size'], t['align'],
                                                                 t['source'], fl or '(no data members)'))
    out.append('')
    return '\n'.join(out)


# ==========================================================================================================
# 7. Checks
# ==========================================================================================================
def verify_src(sections, roots):
    ok = True
    n = 0
    for sec in sections:
        for v in sec.versions:
            lines = read_header_lines(roots[v], sec.path)
            for first, last, emb in sec.ranges:
                for k, e in enumerate(emb):
                    n += 1
                    actual = lines[first + k - 1] if first + k - 1 < len(lines) else '<missing>'
                    if actual != e.rstrip():
                        ok = False
                        print('EXCERPT MISMATCH %s %s:%d\n   embedded: %r\n   header:   %r'
                              % (v, sec.path, first + k, e, actual))
    print('verify-src: %d embedded excerpt lines compared against the headers: %s' % (n, 'OK' if ok else 'FAILED'))
    # CONTRACTS table vs contract_def.h
    for v in VERSIONS:
        text = '\n'.join(read_header_lines(roots[v], 'contract_core/contract_def.h'))
        for c in CONTRACTS:
            idx = c['index'][v]
            if c.get('template'):
                continue
            if idx is None:
                if ('contracts/' + os.path.basename(c['header'])) in text:
                    ok = False
                    print('CONTRACT TABLE MISMATCH %s: %s unexpectedly present' % (v, c['struct']))
                continue
            row = re.search(r'\{"%s",\s*(\d+),\s*(\d+),\s*sizeof\(([\w:]+)\)\}' % re.escape(c['asset']), text)
            inc = re.search(r'#define CONTRACT_STATE_TYPE %s\s*\n#define CONTRACT_STATE2_TYPE %s2\s*\n#include "%s"'
                            % (c['struct'], c['struct'], re.escape(c['header'])), text)
            if c.get('test'):
                good_idx = re.search(r'constexpr unsigned short %s_CONTRACT_INDEX = \(CONTRACT_INDEX \+ 1\);' % c['define'], text)
            else:
                good_idx = re.search(r'#define %s_CONTRACT_INDEX %d\b' % (c['define'], idx), text)
            if not (row and inc and good_idx and int(row.group(1)) == c['constructionEpoch'] and row.group(3) == c['state']):
                ok = False
                print('CONTRACT TABLE MISMATCH %s: %s' % (v, c['struct']))
    print('verify-src: CONTRACTS table vs contract_def.h: %s' % ('OK' if ok else 'FAILED'))
    return ok


def regen_blob(sections, roots):
    out = []
    for sec in sections:
        lines = read_header_lines(roots[sec.versions[0]], sec.path)
        out.append('#@@ file=%s versions=%s' % (sec.path, ','.join(sec.versions)))
        for first, last, _ in sec.ranges:
            out.append('#@ %d-%d' % (first, last))
            out.extend(lines[first - 1:last])
    return '\n'.join(out) + '\n'


CPP_OK = re.compile(r'^[A-Za-z0-9_:<>,\[\] ]+$')


def emit_oracle(result, types, root, version):
    """C++ probe: prints sizeof/alignof for every named type and offsetof for every field."""
    text = '\n'.join(read_header_lines(root, 'contract_core/contract_def.h'))
    a = text.index('#define QX_CONTRACT_INDEX 1')
    b = text.index('#define MAX_CONTRACT_ITERATION_DURATION')
    out = ['// GENERATED by contract_layouts_b.py --emit-oracle %s ; do not edit' % version,
           '#define NO_UEFI',
           '#define INCLUDE_CONTRACT_TEST_EXAMPLES',
           '#include <cstdio>',
           '#include <cstddef>',
           '#include "oracle_shims.h"',
           '#include "platform/memory.h"',
           '#include "contract_core/pre_qpi_def.h"',
           '#include "qpi/qpi.h"',
           '#include "qpi_proposals_impl_patched.h"',
           '#include "oracle_core/oracle_interfaces_def.h"',
           '#include "oc_core/oc_interfaces_def.h"',
           '#include "oracle_shims2.h"',
           text[a:b],
           '#undef CONTRACT_INDEX', '#undef CONTRACT_STATE_TYPE', '#undef CONTRACT_STATE2_TYPE',
           '#define CONTRACT_INDEX 999', '#define CONTRACT_STATE_TYPE CNAME', '#define CONTRACT_STATE2_TYPE CNAME2',
           '#include "contracts/EmptyTemplate.h"',
           'struct IPO { m256i publicKeys[NUMBER_OF_COMPUTORS]; long long prices[NUMBER_OF_COMPUTORS]; };',
           '#define T_(s, T) printf("T\\t%s\\t%zu\\t%zu\\n", s, sizeof(T), alignof(T));',
           '#define F_(s, T, m) printf("F\\t%s\\t%s\\t%zu\\t%zu\\n", s, #m, offsetof(T, m), sizeof(((T*)0)->m));',
           '#define I_(name, value) printf("I\\t%s\\t%d\\n", name, (int)(value));',
           'int main() {']
    for c in result['contracts']:
        if c['index'] is not None:
            out.append('  I_("%s", %s_CONTRACT_INDEX)' % (c['struct'], c['struct']))
    for name, t in types.items():
        if not CPP_OK.match(name) or isinstance(t, RArray):
            continue
        # elaborated type specifier: a nested struct may be hidden by a member FUNCTION of the same name
        # (TestExampleB/C: struct IncomingTransferAmounts vs PUBLIC_FUNCTION(IncomingTransferAmounts))
        kw = 'enum' if isinstance(t, REnum) else ('union' if t.kind == 'union' else 'struct')
        out.append('  { typedef %s %s X_; T_("%s", X_)' % (kw, name, name))
        if isinstance(t, RStruct):
            for f in t.fields:
                if f.name:
                    out.append('    F_("%s", X_, %s)' % (name, f.name))
        out.append('  }')
    out.append('  return 0;\n}')
    return '\n'.join(out) + '\n'


def check_oracle(result, types, path):
    ok, n = True, 0
    by_name = {re.sub(r'\s+', '', k): v for k, v in types.items()}
    idx = {c['struct']: c['index'] for c in result['contracts']}
    for line in open(path):
        p = line.rstrip('\n').split('\t')
        if p[0] == 'T':
            t = by_name[re.sub(r'\s+', '', p[1])]
            n += 1
            if (t.size, t.align) != (int(p[2]), int(p[3])):
                ok = False
                print('ORACLE MISMATCH type %s: computed size/align %d/%d, g++ %s/%s' % (p[1], t.size, t.align, p[2], p[3]))
        elif p[0] == 'F':
            t = by_name[re.sub(r'\s+', '', p[1])]
            f = [x for x in t.fields if x.name == p[2]][0]
            n += 1
            if (f.offset, f.type.size) != (int(p[3]), int(p[4])):
                ok = False
                print('ORACLE MISMATCH field %s::%s: computed offset/size %d/%d, g++ %s/%s'
                      % (p[1], p[2], f.offset, f.type.size, p[3], p[4]))
        elif p[0] == 'I':
            n += 1
            if idx[p[1]] != int(p[2]):
                ok = False
                print('ORACLE MISMATCH contract index %s: table %s, g++ %s' % (p[1], idx[p[1]], p[2]))
    print('check-oracle: %d values compared with the g++ probe output %s: %s' % (n, path, 'OK' if ok else 'FAILED'))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--json')
    ap.add_argument('--markdown')
    ap.add_argument('--verify-src', action='store_true')
    ap.add_argument('--root-old', default=DEFAULT_ROOTS['v1.303.2'])
    ap.add_argument('--root-head', default=DEFAULT_ROOTS['HEAD'])
    ap.add_argument('--state-dir')
    ap.add_argument('--emit-oracle', nargs=2, metavar=('VERSION', 'OUT'))
    ap.add_argument('--check-oracle', nargs=2, metavar=('VERSION', 'FILE'), action='append', default=[])
    ap.add_argument('--regen-blob', action='store_true')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()
    roots = {'v1.303.2': args.root_old, 'HEAD': args.root_head}
    sections = parse_blob(EXCERPTS)
    if args.regen_blob:
        sys.stdout.write(regen_blob(sections, roots))
        return 0
    ok = True
    if args.verify_src:
        ok &= verify_src(sections, roots)
    results, alltypes = {}, {}
    for v in VERSIONS:
        world = World(sections, v)
        results[v], alltypes[v] = build_version(world, v, args.state_dir)
    if not args.quiet:
        for v in VERSIONS:
            print('== %s (core %s, epoch %d)' % (v, results[v]['coreVersion'], results[v]['epoch']))
            print('   %-5s %-13s %-28s %12s %12s  %s' % ('index', 'struct', 'state type', 'stateSize', 'StateData', 'file check'))
            for c in results[v]['contracts']:
                chk = ''
                if 'file' in c:
                    chk = 'file %d bytes: %s' % (c['file']['size'], 'MATCH' if c['file']['matchesStateSize'] else 'MISMATCH')
                    ok &= c['file']['matchesStateSize']
                print('   %-5s %-13s %-28s %12d %12d  %s' % (c['index'] if c['index'] is not None else '-', c['struct'],
                                                             c['stateType'], c['stateSize'], c['stateDataSize'], chk))
    if args.json:
        doc = {
            'schema': 'qstate-viewer/contract-layouts-b/1',
            'generator': 'research/scripts/contract_layouts_b.py',
            'abi': 'x86-64 natural alignment; m256i/id = 32 bytes align 8; uint128 = 16 bytes align 8; '
                   'bit/bool/char = 1; enum = underlying type; struct size rounded up to max member alignment',
            'versions': results,
        }
        with open(args.json, 'w') as f:
            json.dump(doc, f, indent=1)
            f.write('\n')
    if args.markdown:
        with open(args.markdown, 'w') as f:
            for v in VERSIONS:
                f.write(markdown(results[v], v))
                f.write('\n')
    if args.emit_oracle:
        v, outp = args.emit_oracle
        with open(outp, 'w') as f:
            f.write(emit_oracle(results[v], alltypes[v], roots[v], v))
    for v, path in args.check_oracle:
        ok &= check_oracle(results[v], alltypes[v], path)
    return 0 if ok else 1


# ==========================================================================================================
# 8. EXCERPTS: verbatim line ranges of the Qubic core headers ("#@@ file=<path under src/> versions=<list>",
#    "#@ first-last" = 1-based inclusive line range; the lines that follow are copied verbatim, with trailing
#    whitespace / CR removed).  Checked by --verify-src, regenerated by --regen-blob.
# ==========================================================================================================
EXCERPTS = r'''
#@@ file=qpi/qpi_types.h versions=v1.303.2,HEAD
#@ 9-18
namespace QPI
{
	typedef signed char sint8;
	typedef unsigned char uint8;
	typedef signed short sint16;
	typedef unsigned short uint16;
	typedef signed int sint32;
	typedef unsigned int uint32;
	typedef signed long long sint64;
	typedef unsigned long long uint64;
#@ 28-28
#define NUMBER_OF_COMPUTORS 676
#@ 34-46
	struct bit
	{
		bit(bool v = false) : charValue(v)
		{
		}

		operator bool() const
		{
			return !!charValue;
		}

		char charValue;
	};
#@ 61-62
	typedef uint128_t uint128;
	typedef m256i id;
#@ 77-81
	struct Asset
	{
		id issuer;
		uint64 assetName;
	};
#@ 113-113
	struct NoData {};
#@ 119-121
	struct ContractBase
	{
		struct StateData {};
#@ 150-150
	};
#@ 232-233
	constexpr unsigned long long X_MULTIPLIER = 1ULL;
}
#@@ file=qpi/qpi_containers.h versions=v1.303.2,HEAD
#@ 5-6
namespace QPI
{
#@ 8-19
	template <uint64 L>
	struct BitArray
	{
	private:
		static_assert(L && !(L& (L - 1)),
			"The capacity of the BitArray must be 2^N."
			);

		static constexpr uint64 _bits = L;
		static constexpr uint64 _elements = ((L + 63) / 64);

		uint64 _values[_elements];
#@ 84-84
	};
#@ 115-123
	template <typename T, uint64 L>
	struct Array
	{
	private:
		static_assert(L && !(L& (L - 1)),
			"The capacity of the array must be 2^N."
			);

		T _values[L];
#@ 203-203
	};
#@ 252-258
	template <typename T, uint64 L>
	struct SlowAnySizeArray
	{
	private:
		static_assert(L, "The capacity of the array must be != 0.");

		T _values[L];
#@ 300-300
	};
#@ 311-333
	template <typename KeyT, typename ValueT, uint64 L, typename HashFunc = HashFunction<KeyT>>
	class HashMap
	{
	private:
		static_assert(L && !(L& (L - 1)),
			"The capacity of the hash map must be 2^N."
			);
		static constexpr sint64 _nEncodedFlags = L > 32 ? 32 : L;

		// Hash map of (key, value) pairs
		struct Element
		{
			KeyT key;
			ValueT value;
		} _elements[L];

		// 2 bits per element of _elements: 0b00 = not occupied; 0b01 = occupied; 0b10 = occupied but marked for removal; 0b11 is unused
		// The state "occupied but marked for removal" is needed for finding the index of a key in the hash map. Setting an entry to
		// "not occupied" in remove() would potentially undo a collision, create a gap, and mess up the entry search.
		uint64 _occupationFlags[(L * 2 + 63) / 64];

		uint64 _population;
		uint64 _markRemovalCounter;
#@ 405-405
	};
#@ 409-427
	template <typename KeyT, uint64 L, typename HashFunc = HashFunction<KeyT>>
	class HashSet
	{
	private:
		static_assert(L && !(L& (L - 1)),
			"The capacity of the hash set must be 2^N."
			);
		static constexpr sint64 _nEncodedFlags = L > 32 ? 32 : L;

		// Hash set
		KeyT _keys[L];

		// 2 bits per element of _elements: 0b00 = not occupied; 0b01 = occupied; 0b10 = occupied but marked for removal; 0b11 is unused
		// The state "occupied but marked for removal" is needed for finding the index of a key in the hash map. Setting an entry to
		// "not occupied" in remove() would potentially undo a collision, create a gap, and mess up the entry search.
		uint64 _occupationFlags[(L * 2 + 63) / 64];

		uint64 _population;
		uint64 _markRemovalCounter;
#@ 487-487
	};
#@ 492-539
	template <typename T, uint64 L>
	struct Collection
	{
	private:
		static_assert(L && !(L& (L - 1)),
			"The capacity of the Collection must be 2^N."
			);
		static constexpr sint64 _nEncodedFlags = L > 32 ? 32 : L;

		// Hash map of point of views = element filters, each with one priority queue (or empty)
		struct PoV
		{
			id value;
			uint64 population;
			sint64 headIndex, tailIndex;
			sint64 bstRootIndex;
		} _povs[L];

		// 2 bits per element of _povs: 0b00 = not occupied; 0b01 = occupied; 0b10 = occupied but marked for removal; 0b11 is unused
		// The state "occupied but marked for removal" is needed for finding the index of a pov in the hash map. Setting an entry to
		// "not occupied" in remove() would potentially undo a collision, create a gap, and mess up the entry search.
		uint64 _povOccupationFlags[(L * 2 + 63) / 64];

		// Array of elements (filled sequentially), each belongs to one PoV / priority queue (or is empty)
		// Elements of a POV entry will be stored as a binary search tree (BST); so this structure has some properties related to BST
		// (bstParentIndex, bstLeftIndex, bstRightIndex).
		struct Element
		{
			T value;
			sint64 priority;
			sint64 povIndex;
			sint64 bstParentIndex;
			sint64 bstLeftIndex;
			sint64 bstRightIndex;

			Element& init(const T& value, const sint64& priority, const sint64& povIndex)
			{
				this->value = value;
				this->priority = priority;
				this->povIndex = povIndex;
				this->bstParentIndex = NULL_INDEX;
				this->bstLeftIndex = NULL_INDEX;
				this->bstRightIndex = NULL_INDEX;
				return *this;
			}
		} _elements[L];
		uint64 _population;
		uint64 _markRemovalCounter;
#@ 657-657
	};
#@ 664-686
	template <typename T, uint64 L>
	class LinkedList
	{
	private:
		static_assert(L && !(L& (L - 1)),
			"The capacity of the LinkedList must be 2^N."
			);

		struct Node
		{
			T value;
			sint64 nextIndex;  // Next in data list, or next in free list when freed
			sint64 prevIndex;  // Previous in data list (undefined when freed)
		} _nodes[L];

		// 1 bit per node: 1 = occupied, 0 = free
		uint64 _occupiedFlags[(L + 63) / 64];

		sint64 _headIndex;      // First element in the list (NULL_INDEX if empty)
		sint64 _tailIndex;      // Last element in the list (NULL_INDEX if empty)
		sint64 _freeHeadIndex;  // Head of recycled-nodes free list (NULL_INDEX if none)
		uint64 _nextUnusedIndex; // Next never-used node index (lazy free list init)
		uint64 _population;
#@ 752-752
	};
#@ 753-753
}
#@@ file=qpi/qpi_date_time.h versions=v1.303.2,HEAD
#@ 8-9
namespace QPI
{
#@ 15-16
	struct DateAndTime
	{
#@ 599-610
	protected:
		// condensed binary 8-byte representation supporting fast comparison:
		// - padding/reserved: 2 bits (most significant bits in 8-byte number, bits 62-63)
		// - year: 16 bits (bits 46-61)
		// - month: 4 bits (bits 42-45)
		// - day: 5 bits (bits 37-41)
		// - hour: 5 bits (bits 32-36)
		// - minute: 6 bits (bits 26-31)
		// - second: 6 bits (bits 20-25)
		// - millisecond: 10 bits (bits 10-19)
		// - microsecondDuringMillisecond: 10 bits (lowest significance in 8-byte number, bits 0-9)
		uint64 value;
#@ 652-653
	};
}
#@@ file=qpi/qpi_macros.h versions=v1.303.2,HEAD
#@ 7-36
namespace QPI
{
	// Management rights transfer: pre-transfer input
	struct PreManagementRightsTransfer_input
	{
		Asset asset;
		id owner;
		id possessor;
		sint64 numberOfShares;
		sint64 offeredFee;
		uint16 otherContractIndex;
	};

	// Management rights transfer: pre-transfer output (default is all-zeroed = don't allow transfer)
	struct PreManagementRightsTransfer_output
	{
		bool allowTransfer;
		sint64 requestedFee;
	};

	// Management rights transfer: post-transfer input
	struct PostManagementRightsTransfer_input
	{
		Asset asset;
		id owner;
		id possessor;
		sint64 numberOfShares;
		sint64 receivedFee;
		uint16 otherContractIndex;
	};
#@ 572-572
}
#@@ file=qpi/qpi_proposals.h versions=v1.303.2,HEAD
#@ 5-9
namespace QPI
{
	constexpr uint16 INVALID_PROPOSAL_INDEX = 0xffff;
	constexpr uint32 INVALID_VOTE_INDEX = 0xffffffff;
	constexpr sint64 NO_VOTE_VALUE = 0x8000000000000000;
#@ 239-290
	template <bool SupportScalarVotes>
	struct ProposalDataV1
	{
		// URL explaining proposal, zero-terminated string.
		Array<uint8, 256> url;

		// Epoch, when proposal is active. For setProposal(), 0 means to clear proposal and non-zero means the current epoch.
		uint16 epoch;

		// Type of proposal, see ProposalTypes.
		uint16 type;

		// Tick when proposal has been set. Output only, overwritten in setProposal().
		uint32 tick;

		// Proposal payload data (for all except types with class GeneralProposal)
		union Data
		{
			// Used if type class is Transfer
			struct Transfer
			{
				id destination;
				Array<sint64, 4> amounts;   // N first amounts are the proposed options (non-negative, sorted without duplicates), rest zero
			} transfer;

			// Used if type class is TransferInEpoch
			struct TransferInEpoch
			{
				id destination;
				sint64 amount;              // non-negative
				uint16 targetEpoch;         // not checked by isValid()!
			} transferInEpoch;

			// Used if type class is Variable and type is not VariableScalarMean
			struct VariableOptions
			{
				uint64 variable;            // For identifying variable (interpreted by contract only)
				Array<sint64, 4> values;    // N first amounts are proposed options sorted without duplicates, rest zero
			} variableOptions;

			// Used if type is VariableScalarMean
			struct VariableScalar
			{
				uint64 variable;            // For identifying variable (interpreted by contract only)
				sint64 minValue;            // Minimum value allowed in proposedValue and votes, must be > NO_VOTE_VALUE
				sint64 maxValue;            // Maximum value allowed in proposedValue and votes, must be >= minValue
				sint64 proposedValue;       // Needs to be in range between minValue and maxValue

				static constexpr sint64 minSupportedValue = 0x8000000000000001;
				static constexpr sint64 maxSupportedValue = 0x7fffffffffffffff;
			} variableScalar;
		} data;
#@ 349-350
		// Whether to support scalar votes next to option votes.
		static constexpr bool supportScalarVotes = SupportScalarVotes;
#@ 362-362
	};
#@ 367-397
	struct ProposalDataYesNo
	{
		// URL explaining proposal, zero-terminated string.
		Array<uint8, 256> url;

		// Epoch, when proposal is active. For setProposal(), 0 means to clear proposal and non-zero means the current epoch.
		uint16 epoch;

		// Type of proposal, see ProposalTypes.
		uint16 type;

		// Tick when proposal has been set. Output only, overwritten in setProposal().
		uint32 tick;

		// Proposal payload data (for all except types with class GeneralProposal)
		union Data
		{
			// Used if type class is Transfer
			struct Transfer
			{
				id destination;
				sint64 amount;		// Amount of proposed option (non-negative)
			} transfer;

			// Used if type class is Variable and type is not VariableScalarMean
			struct VariableOptions
			{
				uint64 variable;    // For identifying variable (interpreted by contract only)
				sint64 value;		// Value of proposed option, rest zero
			} variableOptions;
		} data;
#@ 423-424
		// Whether to support scalar votes next to option votes.
		static constexpr bool supportScalarVotes = false;
#@ 436-436
	};
#@ 472-499
	template <typename ProposerAndVoterHandlingT, typename ProposalDataT>
	class ProposalVoting
	{
	public:
		static constexpr uint16 maxProposals = ProposerAndVoterHandlingT::maxProposals;
		static constexpr uint32 maxVotes = ProposerAndVoterHandlingT::maxVotes;

		typedef ProposerAndVoterHandlingT ProposerAndVoterHandlingType;
		typedef ProposalDataT ProposalDataType;
		typedef ProposalWithAllVoteData<
			ProposalDataT,
			maxVotes
		> ProposalAndVotesDataType;

		static_assert(maxProposals <= INVALID_PROPOSAL_INDEX);
		static_assert(maxVotes <= INVALID_VOTE_INDEX);

		// Handling of who has the right to propose and to vote + proposal / voter indices
		ProposerAndVoterHandlingType proposersAndVoters;

	protected:
		// Proposals and corresponding votes. No direct access for contracts.
		ProposalAndVotesDataType proposals[maxProposals];

		// Give user interface access to proposals
		friend struct QpiContextProposalProcedureCall<ProposerAndVoterHandlingT, ProposalDataT>;
		friend struct QpiContextProposalFunctionCall<ProposerAndVoterHandlingT, ProposalDataT>;
	};
#@ 621-621
}
#@@ file=qpi/impl/qpi_proposals_impl.h versions=v1.303.2,HEAD
#@ 5-6
namespace QPI
{
#@ 8-15
	template <uint16 proposalSlotCount>
	struct ProposalAndVotingByComputors
	{
		// Maximum number of proposals (may be lower than number of proposers = IDs with right to propose and lower than num. of voters)
		static constexpr uint16 maxProposals = proposalSlotCount;

		// Maximum number of voters / votes (each computor has one vote)
		static constexpr uint32 maxVotes = NUMBER_OF_COMPUTORS;
#@ 112-119
	protected:
		// needs to be initialized with zeros
		id currentProposalProposers[maxProposals];
	};

	template <uint16 proposalSlotCount>
	struct ProposalByAnyoneVotingByComputors : public ProposalAndVotingByComputors<proposalSlotCount>
	{
#@ 125-125
	};
#@ 130-137
	template <uint16 proposalSlotCount, uint64 contractAssetName>
	struct ProposalAndVotingByShareholders
	{
		// Maximum number of proposals (may be lower than number of proposers = IDs with right to propose and lower than num. of voters)
		static constexpr uint16 maxProposals = proposalSlotCount;

		// Maximum number of votes (676 shares per contract)
		static constexpr uint32 maxVotes = NUMBER_OF_COMPUTORS;
#@ 336-340
	protected:
		// needs to be initialized with zeros
		id currentProposalProposers[maxProposals];
		id currentProposalShareholders[maxProposals][NUMBER_OF_COMPUTORS];
	};
#@ 370-373
	template <bool scalarVotesSupported>
	struct __VoteStorageTypeSelector { typedef uint8 type;  };
	template <>
	struct __VoteStorageTypeSelector<true> { typedef sint64 type; };
#@ 378-386
	template <typename ProposalDataType, uint32 numOfVotes>
	struct ProposalWithAllVoteData : public ProposalDataType
	{
		// Select type for storage (sint64 if scalar votes are supported, uint8 otherwise).
		static constexpr bool supportScalarVotes = ProposalDataType::supportScalarVotes;
		typedef __VoteStorageTypeSelector<supportScalarVotes>::type VoteStorageType;

		// Vote storage
		VoteStorageType votes[numOfVotes];
#@ 477-477
	};
#@ 481-485
	template <uint32 numOfVotes>
	struct ProposalWithAllVoteData<ProposalDataYesNo, numOfVotes> : public ProposalDataYesNo
	{
		// Vote storage (2 bit per vote)
		uint8 votes[(2 * numOfVotes + 7) / 8];
#@ 545-545
	};
#@ 1123-1123
}
#@@ file=contracts/RandomLottery.h versions=v1.303.2,HEAD
#@ 20-20
using namespace QPI;
#@ 23-23
constexpr uint16 RL_MAX_NUMBER_OF_PLAYERS = 1024;
#@ 26-26
constexpr uint16 RL_MAX_NUMBER_OF_WINNERS_IN_HISTORY = 1024;
#@ 69-71
struct RL : public ContractBase
{
public:
#@ 77-80
	enum class EState : uint8
	{
		SELLING = 1 << 0, // Ticket selling is open
	};
#@ 95-207
	struct WinnerInfo
	{
		id winnerAddress; // Winner address
		uint64 revenue;   // Payout value sent to the winner for that epoch
		uint32 tick;      // Tick when the decision was made
		uint16 epoch;     // Epoch number when winner was recorded
		uint8 dayOfWeek;  // Day of week when the winner was drawn [0..6] 0 = WEDNESDAY
	};

	struct NextEpochData
	{
		uint64 newPrice; // Ticket price to apply after END_EPOCH; 0 means "no change queued"
		uint8 schedule;  // Schedule bitmask (bit 0 = WEDNESDAY, ..., bit 6 = TUESDAY); applied after END_EPOCH
	};

	struct StateData
	{
		/**
		 * @brief Circular buffer storing the history of winners.
		 * Maximum capacity is defined by RL_MAX_NUMBER_OF_WINNERS_IN_HISTORY.
		 */
		Array<WinnerInfo, RL_MAX_NUMBER_OF_WINNERS_IN_HISTORY> winners;

		/**
		 * @brief Set of players participating in the current lottery epoch.
		 * Maximum capacity is defined by RL_MAX_NUMBER_OF_PLAYERS.
		 */
		Array<id, RL_MAX_NUMBER_OF_PLAYERS> players;

		/**
		 * @brief Address of the team managing the lottery contract.
		 * Initialized to a zero address.
		 */
		id teamAddress;

		/**
		 * @brief Address of the owner of the lottery contract.
		 * Initialized to a zero address.
		 */
		id ownerAddress;

		/**
		 * @brief Data structure for deferred changes to apply at the end of the epoch.
		 */
		NextEpochData nexEpochData;

		/**
		 * @brief Price of a single lottery ticket.
		 * Value is in the smallest currency unit (e.g., cents).
		 */
		uint64 ticketPrice;

		/**
		 * @brief Number of players (tickets sold) in the current epoch.
		 */
		uint64 playerCounter;

		/**
		 * @brief Index pointing to the next empty slot in the winners array.
		 * Used for maintaining the circular buffer of winners.
		 */
		uint64 winnersCounter;

		/**
		 * @brief Date/time guard for draw operations.
		 * lastDrawDateStamp prevents more than one action per calendar day (UTC).
		 */
		uint8 lastDrawDay;
		uint8 lastDrawHour;
		uint32 lastDrawDateStamp; // Compact YYYY/MM/DD marker

		/**
		 * @brief Percentage of the revenue allocated to the team.
		 * Value is between 0 and 100.
		 */
		uint8 teamFeePercent;

		/**
		 * @brief Percentage of the revenue allocated for distribution.
		 * Value is between 0 and 100.
		 */
		uint8 distributionFeePercent;

		/**
		 * @brief Percentage of the revenue allocated to the winner.
		 * Automatically calculated as the remainder after other fees.
		 */
		uint8 winnerFeePercent;

		/**
		 * @brief Percentage of the revenue to be burned.
		 * Value is between 0 and 100.
		 */
		uint8 burnPercent;

		/**
		 * @brief Schedule bitmask: bit 0 = WEDNESDAY, 1 = THURSDAY, ..., 6 = TUESDAY.
		 * If a bit is set, a draw may occur on that day (subject to drawHour and daily guard).
		 * Wednesday also follows the "Two-Wednesdays rule" (selling stays closed after Wednesday draw).
		 */
		uint8 schedule;

		/**
		 * @brief UTC hour [0..23] when a draw is allowed to run (daily time gate).
		 */
		uint8 drawHour;

		/**
		 * @brief Current state of the lottery contract.
		 * SELLING: tickets available; LOCKED: selling closed.
		 */
		EState currentState;
	};
#@ 1072-1072
};
#@@ file=contracts/QBond.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 3-3
constexpr uint64 QBOND_MAX_EPOCH_COUNT = 1024ULL;
#@ 29-30
struct QBOND : public ContractBase
{
#@ 37-50
    struct MBondInfo
    {
        uint64 name;
        sint64 stakersAmount;
        sint64 totalStaked;
    };

    struct Order
    {
        id owner;
        sint64 epoch;
        sint64 numberOfMBonds;
        sint64 feeDebt;
    };
#@ 52-52
    typedef Order _Order;
#@ 65-79
    struct StateData
    {
        HashMap<uint16, MBondInfo, QBOND_MAX_EPOCH_COUNT> _epochMbondInfoMap;
        HashMap<id, sint64, 524288> _userTotalStakedMap;
        HashSet<id, 1024> _commissionFreeAddresses;
        uint64 _qearnIncomeAmount;
        uint64 _totalEarnedAmount;
        uint64 _earnedAmountFromTrade;
        uint64 _distributedAmount;
        id _adminAddress;
        id _devAddress;
        Collection<Order, 1048576> _askOrders;
        Collection<Order, 1048576> _bidOrders;
        uint8 _cyclicMbondCounter;
    };
#@ 1428-1428
};
#@@ file=contracts/QIP.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 3-3
constexpr uint32 QIP_MAX_NUMBER_OF_ICO = 16384;
#@ 48-108
struct QIP : public ContractBase
{
public:
    struct BuyerInfo
    {
        sint64 toReceive;
        sint64 received;
        sint64 totalQuPayed;
        bit isReturned;
    };

    struct IcoBuyerKey
    {
        id creator;
        id issuer;
        uint64 assetName;
        id buyer;

        bool operator==(const IcoBuyerKey other) const
        {
            return creator == other.creator
                    && issuer == other.issuer
                    && assetName == other.assetName
                    && buyer == other.buyer;
        }
    };

    struct ICOInfo
    {
        id creatorOfICO;
        id issuer;
        id address1, address2, address3, address4, address5, address6, address7, address8, address9, address10;
        uint64 assetName;
        uint64 price1;
        uint64 price2;
        uint64 price3;
        uint64 saleAmountForPhase1;
        uint64 saleAmountForPhase2;
        uint64 saleAmountForPhase3;
        uint64 remainingAmountForPhase1;
        uint64 remainingAmountForPhase2;
        uint64 remainingAmountForPhase3;
        uint32 percent1, percent2, percent3, percent4, percent5, percent6, percent7, percent8, percent9, percent10;
        uint32 startEpoch;
        bit burnRemainingTokens;
        bit isVested;
        uint32 vestingPeriod;
        uint64 distributedQu;
        uint64 quToDistribute;
        uint64 returnedQu;
    };

    struct StateData
    {
        Array<ICOInfo, QIP_MAX_NUMBER_OF_ICO> icos;
        HashSet<uint64, QIP_MAX_NUMBER_OF_ICO> activeIcoIndexes;
        uint32 currentIcoIndex;
        uint32 transferRightsFee;
        id developmentFundAddress;
        HashMap<IcoBuyerKey, BuyerInfo, 131072> buyersInfo;
    };
#@ 972-972
};
#@@ file=contracts/QRaffle.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 26-36
constexpr uint32 QRAFFLE_MAX_EPOCH = 65536;
constexpr uint32 QRAFFLE_MAX_PROPOSAL_EPOCH = 128;
constexpr uint32 QRAFFLE_MAX_MEMBER = 65536;
constexpr uint32 QRAFFLE_DEFAULT_QRAFFLE_AMOUNT = 1000000ull;
constexpr uint32 QRAFFLE_MIN_QRAFFLE_AMOUNT = 1000000ull;
constexpr uint32 QRAFFLE_MAX_QRAFFLE_AMOUNT = 1000000000ull;
// Ended token-raffle ring: 16 384 slots × ~96 B ≈ 1.5 MB.
// At most QRAFFLE_MAX_PROPOSAL_EPOCH (128) raffles/epoch → covers ~128 epochs of history.
constexpr uint32 QRAFFLE_MAX_TOKEN_RAFFLES = 16384;
constexpr uint32 QRAFFLE_TOKEN_RAFFLE_SLOT_SIZE = 512; // 2^9, max members per token raffle
constexpr uint8 QRAFFLE_MAX_PROPOSALS_PER_PROPOSER = 3; // max proposals per user per epoch
#@ 71-76
constexpr uint32 QRAFFLE_MAX_ASSET_RAFFLES_PER_EPOCH     = 64;              // concurrent active raffles
constexpr uint32 QRAFFLE_MAX_ASSETS_PER_BUNDLE           = 4;               // items per bundle
constexpr uint32 QRAFFLE_MAX_ASSET_TICKET_BUYERS         = 1024;            // distinct buyers per raffle
constexpr uint32 QRAFFLE_MAX_TICKETS_PER_BUYER           = 100;             // per-buyer cap (anti-griefing)
constexpr uint32 QRAFFLE_MAX_ASSET_RAFFLES_PER_CREATOR   = 2;               // per creator per epoch
constexpr uint32 QRAFFLE_MAX_ENDED_ASSET_RAFFLES         = 8192;            // history ring buffer
#@ 80-81
constexpr uint32 QRAFFLE_ASSET_RAFFLE_BUNDLE_FLAT_SIZE   = QRAFFLE_MAX_ASSET_RAFFLES_PER_EPOCH * QRAFFLE_MAX_ASSETS_PER_BUNDLE;   // 256
constexpr uint32 QRAFFLE_ASSET_RAFFLE_BUYERS_FLAT_SIZE   = QRAFFLE_MAX_ASSET_RAFFLES_PER_EPOCH * QRAFFLE_MAX_ASSET_TICKET_BUYERS; // 65536
#@ 84-84
constexpr uint32 QRAFFLE_MAX_SHAREHOLDERS_OLD = 1024;
#@ 91-93
struct QRAFFLE : public ContractBase
{
public:
#@ 252-419
	struct AssetRaffleItem
	{
		Asset  asset;
		sint64 numberOfShares;
	};

	// Active asset raffle state (live during the epoch it was created).
	struct AssetRaffleInfo
	{
		id     creator;
		uint64 reservePriceQu;     // net Qu creator wants AFTER 20% fee
		uint64 entryTicketQu;      // Qu per ticket
		uint64 totalTicketsPaidQu; // gross Qu pool so far
		uint32 numberOfBuyers;
		uint32 totalTickets;
		uint32 bundleSize;
		uint32 epoch;
	};

	// Historical record written at END_EPOCH for each settled asset raffle.
	// Field order keeps the largest types first so trailing padding is minimal and
	// deterministic across compilers (no explicit pad needed → no plain C arrays).
	struct EndedAssetRaffleInfo
	{
		id     creator;
		id     epochWinner;      // NULL_ID if reserve was missed
		uint64 reservePriceQu;
		uint64 entryTicketQu;
		uint64 grossPoolQu;
		uint64 creatorPaidQu;    // 0 if reserve missed
		uint32 totalTickets;
		uint32 numberOfBuyers;
		uint32 bundleSize;
		uint32 epoch;
		uint32 reserveMet;       // 1 = reserve met and winner paid; 0 = refunded (uint32 keeps natural alignment)
	};

	struct ProposalInfo {
		Asset token;
		id proposer;
		uint64 entryAmount;
		uint32 nYes;
		uint32 nNo;
	};

	struct QuRaffleInfo
	{
		id epochWinner;
		uint64 receivedAmount;
		uint64 entryAmount;
		uint32 numberOfMembers;
		uint32 winnerIndex;
	};

	struct TokenRaffleInfo
	{
		id epochWinner;
		Asset token;
		uint64 entryAmount;
		uint32 numberOfMembers;
		uint32 winnerIndex;
		uint32 epoch;
	};

	struct ActiveTokenRaffleInfo {
		Asset token;
		uint64 entryAmount;
	};

	struct OldStateData
	{
		HashMap <id, uint8, QRAFFLE_MAX_MEMBER> registers;
		Array <ProposalInfo, QRAFFLE_MAX_PROPOSAL_EPOCH> proposals;

		HashMap <id, BitArray<QRAFFLE_MAX_PROPOSAL_EPOCH>, QRAFFLE_MAX_MEMBER> voteParticipation;
		HashMap <id, BitArray<QRAFFLE_MAX_PROPOSAL_EPOCH>, QRAFFLE_MAX_MEMBER> voteValues;
		Array <uint32, QRAFFLE_MAX_PROPOSAL_EPOCH> numberOfVotedInProposal;
		Array <id, QRAFFLE_MAX_MEMBER> quRaffleMembers;
		HashSet <id, QRAFFLE_MAX_MEMBER> quRaffleMemberSet;

		Array <ActiveTokenRaffleInfo, QRAFFLE_MAX_PROPOSAL_EPOCH> activeTokenRaffle;
		HashMap <id, BitArray<QRAFFLE_MAX_PROPOSAL_EPOCH>, QRAFFLE_MAX_MEMBER> tokenRaffleParticipation;
		Array <id, QRAFFLE_MAX_MEMBER> tokenRaffleMemberSlots;
		Array <uint32, QRAFFLE_MAX_PROPOSAL_EPOCH> numberOfTokenRaffleMembers;

		Array <QuRaffleInfo, QRAFFLE_MAX_EPOCH> QuRaffles;
		Array <TokenRaffleInfo, QRAFFLE_MAX_TOKEN_RAFFLES> tokenRaffle;
		HashMap <id, uint64, QRAFFLE_MAX_MEMBER> quRaffleEntryAmount;
		HashSet <id, QRAFFLE_MAX_SHAREHOLDERS_OLD> shareholdersList;

		id initialRegister1, initialRegister2, initialRegister3, initialRegister4, initialRegister5;
		id charityAddress, feeAddress, QXMRIssuer;
		uint64 epochRevenue, epochQXMRRevenue, qREAmount, totalBurnAmount, totalCharityAmount, totalShareholderAmount, totalRegisterAmount, totalFeeAmount, totalWinnerAmount, largestWinnerAmount;
		uint32 numberOfRegisters, numberOfQuRaffleMembers, numberOfEntryAmountSubmitted, numberOfProposals, numberOfActiveTokenRaffle, numberOfEndedTokenRaffle;
		Array<uint32, QRAFFLE_MAX_EPOCH> daoMemberCount;
		HashMap <id, uint8, QRAFFLE_MAX_MEMBER> proposalsPerProposer;
	};

	struct StateData
	{
		HashMap <id, uint8, QRAFFLE_MAX_MEMBER> registers;
		Array <ProposalInfo, QRAFFLE_MAX_PROPOSAL_EPOCH> proposals;

		// Per-user vote tracking with dual BitArray (qRWA pattern).
		// O(1) lookup via id hash, 1 bit per proposal. ~4 MB each, ~8 MB total.
		HashMap <id, BitArray<QRAFFLE_MAX_PROPOSAL_EPOCH>, QRAFFLE_MAX_MEMBER> voteParticipation; // bit=1 if user has voted
		HashMap <id, BitArray<QRAFFLE_MAX_PROPOSAL_EPOCH>, QRAFFLE_MAX_MEMBER> voteValues; // bit=1 for yes, bit=0 for no
		Array <uint32, QRAFFLE_MAX_PROPOSAL_EPOCH> numberOfVotedInProposal;
		Array <id, QRAFFLE_MAX_MEMBER> quRaffleMembers;
		// O(1) duplicate guard for quRaffle entries; mirrors quRaffleMembers for membership tests.
		HashSet <id, QRAFFLE_MAX_MEMBER> quRaffleMemberSet;

		Array <ActiveTokenRaffleInfo, QRAFFLE_MAX_PROPOSAL_EPOCH> activeTokenRaffle;
		// Per-user O(1) duplicate check for token raffle deposits. ~4 MB.
		HashMap <id, BitArray<QRAFFLE_MAX_PROPOSAL_EPOCH>, QRAFFLE_MAX_MEMBER> tokenRaffleParticipation;
		// Flat indexed member storage: raffle i occupies slots [i*SLOT_SIZE .. i*SLOT_SIZE+count). ~2 MB.
		Array <id, QRAFFLE_MAX_MEMBER> tokenRaffleMemberSlots;
		Array <uint32, QRAFFLE_MAX_PROPOSAL_EPOCH> numberOfTokenRaffleMembers;

		Array <QuRaffleInfo, QRAFFLE_MAX_EPOCH> QuRaffles;
		Array <TokenRaffleInfo, QRAFFLE_MAX_TOKEN_RAFFLES> tokenRaffle;
		HashMap <id, uint64, QRAFFLE_MAX_MEMBER> quRaffleEntryAmount;

		id initialRegister1, initialRegister2, initialRegister3, initialRegister4, initialRegister5;
		id charityAddress, feeAddress, QXMRIssuer;
		uint64 epochRevenue, epochQXMRRevenue, qREAmount, totalBurnAmount, totalCharityAmount, totalShareholderAmount, totalRegisterAmount, totalFeeAmount, totalWinnerAmount, largestWinnerAmount;
		uint32 numberOfRegisters, numberOfQuRaffleMembers, numberOfEntryAmountSubmitted, numberOfProposals, numberOfActiveTokenRaffle, numberOfEndedTokenRaffle;
		Array<uint32, QRAFFLE_MAX_EPOCH> daoMemberCount; // Number of DAO members (registers) at each epoch
		HashMap <id, uint8, QRAFFLE_MAX_MEMBER> proposalsPerProposer;

		// ── Asset Raffle state ────────────────────────────────────────────────────────
		Array<AssetRaffleInfo, QRAFFLE_MAX_ASSET_RAFFLES_PER_EPOCH> activeAssetRaffles;
		uint32 numberOfActiveAssetRaffles;

		// Bundle items: raffle i occupies [i*4 .. i*4+bundleSize).
		Array<AssetRaffleItem, QRAFFLE_ASSET_RAFFLE_BUNDLE_FLAT_SIZE> activeAssetRaffleItems;

		// Buyer lists: raffle i occupies [i*1024 .. i*1024+numberOfBuyers).
		Array<id,     QRAFFLE_ASSET_RAFFLE_BUYERS_FLAT_SIZE> activeAssetRaffleBuyers;
		Array<uint32, QRAFFLE_ASSET_RAFFLE_BUYERS_FLAT_SIZE> activeAssetRaffleBuyerTickets;

		// O(1) has-bought check: bit[i]=1 means this user has tickets in raffle i.
		HashMap<id, BitArray<QRAFFLE_MAX_ASSET_RAFFLES_PER_EPOCH>, QRAFFLE_MAX_MEMBER> assetRaffleParticipation;

		// O(1) slot lookup: entry[i] = buyer's 0-based position in raffle i's buyer region.
		// Sentinel 0xFFFF = not present. ~8 MB (64 raffles × 2 B × 65536 buyers).
		// Reset each epoch alongside the buyer arrays.
		HashMap<id, Array<uint16, QRAFFLE_MAX_ASSET_RAFFLES_PER_EPOCH>, QRAFFLE_MAX_MEMBER> assetRaffleBuyerSlotIndex;

		// Per-creator raffle count; reset each epoch to enforce QRAFFLE_MAX_ASSET_RAFFLES_PER_CREATOR.
		HashMap<id, uint8, QRAFFLE_MAX_MEMBER> assetRafflesPerCreator;

		// Settled raffle history ring buffer.
		Array<EndedAssetRaffleInfo, QRAFFLE_MAX_ENDED_ASSET_RAFFLES> endedAssetRaffles;
		uint32 numberOfEndedAssetRaffles;

		// Accumulated proposal fees destined for DAO registers (50% of each 500K fee);
		// distributed in one O(R) pass at END_EPOCH alongside the register share bucket.
		uint64 epochAssetRaffleDaoBucket;

		// Aggregate analytics (monotonically increasing).
		uint64 totalAssetRaffleProposalFees;
		uint64 totalAssetRaffleCreatorPaid;
		uint64 totalAssetRaffleRefunded;
		uint32 totalAssetRafflesCreated;
		uint32 totalAssetRafflesSucceeded;
		uint32 totalAssetRafflesFailed;
	};
#@ 2876-2876
};
#@@ file=contracts/qRWA.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 7-10
constexpr uint64 QRWA_MAX_QMINE_HOLDERS = 131072 * X_MULTIPLIER; // 2^17 = 128K unique holders max (563MB → 37MB state)
constexpr uint64 QRWA_MAX_GOV_POLLS = 64; // 8 active polls * 8 epochs = 64 slots

constexpr uint64 QRWA_MAX_ASSETS = 1024; // 2^10
#@ 80-80
constexpr uint64 QRWA_PAYOUT_RING_SIZE = 16384; // Must be a power of 2
#@ 99-100
struct QRWA : public ContractBase
{
#@ 106-157
    struct QRWAAsset
    {
        id issuer;
        uint64 assetName;

        operator Asset() const
        {
            return { issuer, assetName };
        }

        bool operator==(const QRWAAsset other) const
        {
            return issuer == other.issuer && assetName == other.assetName;
        }

        bool operator!=(const QRWAAsset other) const
        {
            return issuer != other.issuer || assetName != other.assetName;
        }

        inline void setFrom(const Asset& asset)
        {
            issuer = asset.issuer;
            assetName = asset.assetName;
        }
    };

    // votable governance parameters for the contract.
    struct QRWAGovParams
    {
        // Addresses
        id mAdminAddress; // Only the admin can create release polls
        // Addresses to receive the MINING FEEs
        id electricityAddress;
        id maintenanceAddress;
        id reinvestmentAddress;
        id qmineDevAddress; // Address to receive rewards for moved QMINE during epoch (NOT changeable via voting)

        // MINING FEE Percentages
        uint64 electricityPercent;
        uint64 maintenancePercent;
        uint64 reinvestmentPercent;
    };

    // Represents a governance poll in a rotating buffer
    struct QRWAGovProposal
    {
        uint64 proposalId; // The unique, increasing ID
        uint64 status; // 0=Empty, 1=Active, 2=Passed, 3=Failed
        uint64 score; // Final score, count at END_EPOCH
        QRWAGovParams params; // The actual proposal data
    };
#@ 173-183
    struct QRWAPayoutEntry
    {
        id recipient;          // Who received the payment
        uint64 amount;         // Amount in QU
        uint64 qmineHolding;   // Recipient's QMINE shares at payout time
        uint64 qrwaHolding;    // Recipient's qRWA shares at payout time
        uint32 tick;           // Network tick of the payout
        uint16 epoch;          // Epoch of the payout
        uint8 payoutType;      // QRWA_PAYOUT_TYPE_* constant
        uint8 _pad0;
    };
#@ 190-280
    struct StateData
    {
        Asset mQmineAsset;

        // QMINE Shareholder Tracking
        HashMap<id, uint64, QRWA_MAX_QMINE_HOLDERS> mBeginEpochBalances;
        HashMap<id, uint64, QRWA_MAX_QMINE_HOLDERS> mEndEpochBalances;
        uint64 mTotalQmineBeginEpoch; // Total QMINE shares at the start of the current epoch

        // PAYOUT SNAPSHOTS (for distribution)
        // These hold the data from the last epoch, saved at END_EPOCH
        HashMap<id, uint64, QRWA_MAX_QMINE_HOLDERS> mPayoutBeginBalances;
        HashMap<id, uint64, QRWA_MAX_QMINE_HOLDERS> mPayoutEndBalances;
        uint64 mPayoutTotalQmineBegin; // Total QMINE shares from the last epoch's beginning

        // Votable Parameters
        QRWAGovParams mCurrentGovParams; // The live, active parameters

        // Voting state for governance parameters (voted by QMINE holders)
        Array<QRWAGovProposal, QRWA_MAX_GOV_POLLS> mGovPolls;
        HashMap<id, uint64, QRWA_MAX_QMINE_HOLDERS> mShareholderVoteMap; // Maps QMINE holder -> Gov Poll slot index
        uint64 mCurrentGovProposalId;
        uint64 mNewGovPollsThisEpoch;



        // Treasury & Asset Release
        uint64 mTreasuryBalance; // QMINE token balance holds by SC
        HashMap<QRWAAsset, uint64, QRWA_MAX_ASSETS> mGeneralAssetBalances; // Balances for other assets (e.g., SC shares)
        HashMap<id, uint64, QRWA_MAX_ASSETS> mScDividendTracker; // SC contract ID → cumulative dividends received (routed to Pool B)

        // Per-pool payout timestamps (UTC time-based, used when QRWA_USE_TICK_BASED_PAYOUT == 0)
        DateAndTime mLastPayoutTimePoolA; // Pool A (Qubic Mining)
        DateAndTime mLastPayoutTimePoolB; // Pool B (SC Assets)
        DateAndTime mLastPayoutTimePoolC; // Pool C (BTC Mining)
        DateAndTime mLastPayoutTimePoolD; // Pool D (MLM Water)

        // Per-pool payout tick tracking (used when QRWA_USE_TICK_BASED_PAYOUT == 1)
        uint32 mLastPayoutTickPoolA;
        uint32 mLastPayoutTickPoolB;
        uint32 mLastPayoutTickPoolC;
        uint32 mLastPayoutTickPoolD;
        uint32 _paddingTick; // Alignment padding

        // Revenue Pools (incoming, before splitting into QMINE/qRWA)
        uint64 mRevenuePoolA; // Mined funds from Qubic farm (from SCs) — gov fees deducted first
        uint64 mRevenuePoolB; // Other dividend funds (from user wallets) — no gov fees
        uint64 mDedicatedRevenuePool; // Pool C (BTC Mining) revenue from dedicated address
        uint64 mPoolDRevenuePool; // Pool D (MLM Water) revenue from dedicated address

        // Per-pool dividend sub-pools (populated from revenue, split 90% QMINE / 10% qRWA)
        uint64 mPoolAQmineDividend;    // 90% of Pool A revenue (after gov fees)
        uint64 mPoolAQrwaDividend;     // 10% of Pool A revenue (after gov fees)
        uint64 mPoolBQmineDividend;    // 90% of Pool B revenue (no gov fees)
        uint64 mPoolBQrwaDividend;     // 10% of Pool B revenue (no gov fees)
        uint64 mPoolCQmineDividend;    // 90% of Pool C revenue
        uint64 mPoolCQrwaDividend;     // 10% of Pool C revenue (dedicated, requires >= 100K QMINE/share)
        uint64 mPoolDQmineDividend;    // 80% of Pool D revenue (after 10% reinvestment)
        uint64 mPoolDQrwaDividend;     // 10% of Pool D revenue (dedicated, requires >= 100K QMINE/share)

        // Pool C (BTC Mining) revenue configuration
        id mDedicatedRevenueAddress;

        // Pool D (MLM Water) revenue configuration
        id mPoolDRevenueAddress;

        // Pool A revenue address (QMINE issuer or configured mining address)
        id mPoolARevenueAddress;

        // Fundraising address — excluded from ALL distributions
        id mFundraisingAddress;

        // Exchange address (safe.trade) — excluded from ALL distributions
        id mExchangeAddress;

        // Per-pool total distributed tracking
        uint64 mTotalPoolADistributed;
        uint64 mTotalPoolBDistributed;
        uint64 mTotalPoolCDistributed;
        uint64 mTotalPoolDDistributed;

        // Per-pool ring buffers (one per pool, each contains all payout types for that pool)
        Array<QRWAPayoutEntry, QRWA_PAYOUT_RING_SIZE> mPayoutsPoolA;   // Pool A: QMINE + qRWA payouts
        uint16 mPayoutsPoolANextIdx;
        Array<QRWAPayoutEntry, QRWA_PAYOUT_RING_SIZE> mPayoutsPoolB;   // Pool B: QMINE + qRWA payouts
        uint16 mPayoutsPoolBNextIdx;
        Array<QRWAPayoutEntry, QRWA_PAYOUT_RING_SIZE> mPayoutsPoolC;   // Pool C: QMINE + dedicated qRWA payouts
        uint16 mPayoutsPoolCNextIdx;
        Array<QRWAPayoutEntry, QRWA_PAYOUT_RING_SIZE> mPayoutsPoolD;   // Pool D: QMINE + dedicated qRWA payouts (MLM Water)
        uint16 mPayoutsPoolDNextIdx;
    }; // end StateData
#@ 2860-2860
};
#@@ file=contracts/QReservePool.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 3-3
constexpr uint16 QRP_ALLOWED_SC_NUM = 128;
#@ 11-28
struct QRP : ContractBase
{
	struct StateData
	{
		/**
		 * @brief Address of the team managing the lottery contract.
		 * Initialized to a zero address.
		 */
		id teamAddress;

		/**
		 * @brief Address of the owner of the lottery contract.
		 * Initialized to a zero address.
		 */
		id ownerAddress;

		HashSet<id, QRP_ALLOWED_SC_NUM> allowedSmartContracts;
	};
#@ 248-248
};
#@@ file=contracts/QThirtyFour.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 4-12
constexpr uint64 QTF_MAX_NUMBER_OF_PLAYERS = 1024;
constexpr uint64 QTF_RANDOM_VALUES_COUNT = 4;
constexpr uint64 QTF_MAX_RANDOM_VALUE = 30;
constexpr uint64 QTF_MAX_RANDOM_VALUE_ALIGNED = QTF_MAX_RANDOM_VALUE + 2;
constexpr uint64 QTF_MAX_BATCH_TICKETS = div(QTF_MAX_NUMBER_OF_PLAYERS, 4ULL);
constexpr uint64 QTF_BATCH_TICKET_VALUES_COUNT = QTF_MAX_BATCH_TICKETS * QTF_RANDOM_VALUES_COUNT;
constexpr uint64 QTF_TICKET_PRICE = 1000000;
constexpr uint64 QTF_WINNING_COMBINATIONS_HISTORY_SIZE = 128;
constexpr uint64 QTF_PREPARED_TICKET_MAX_COUNT = QTF_MAX_NUMBER_OF_PLAYERS * QTF_RANDOM_VALUES_COUNT;
#@ 75-79
struct QTF : ContractBase
{
	// Forward declaration for NextEpochData::apply
	struct StateData;

#@ 94-98
	enum EState : uint8
	{
		STATE_NONE = 0,
		STATE_SELLING = 1 << 0
	};
#@ 100-165
	struct PlayerData
	{
		id player;
		Array<uint8, QTF_RANDOM_VALUES_COUNT> randomValues;

		bool isValid() const { return !isZero(player); }
	};

	struct WinnerPlayerData
	{
		id player;
		Array<uint8, QTF_RANDOM_VALUES_COUNT> randomValues;
		uint32 wonAmount;

		void addPlayerData(const PlayerData& data)
		{
			player = data.player;
			randomValues = data.randomValues;
		}

		bool isValid() const { return !isZero(player); }
	};

	struct WinnerData
	{
		Array<WinnerPlayerData, QTF_MAX_NUMBER_OF_PLAYERS> winners;
		Array<uint8, QTF_RANDOM_VALUES_COUNT> winnerValues;
		uint64 winnerCounter;
		uint16 epoch;
	};

	struct NextEpochData
	{
		void clear()
		{
			newTicketPrice = 0;
			newTargetJackpot = 0;
			newSchedule = 0;
			newDrawHour = 0;
		}

		void apply(QPI::ContractState<StateData, CONTRACT_INDEX>& state) const
		{
			if (newTicketPrice > 0)
			{
				state.mut().ticketPrice = newTicketPrice;
			}
			if (newTargetJackpot > 0)
			{
				state.mut().targetJackpot = newTargetJackpot;
			}
			if (newSchedule > 0)
			{
				state.mut().schedule = newSchedule;
			}
			if (newDrawHour > 0)
			{
				state.mut().drawHour = newDrawHour;
			}
		}

		uint64 newTicketPrice;
		uint64 newTargetJackpot;
		uint8 newSchedule;
		uint8 newDrawHour;
	};
#@ 867-889
	struct StateData
	{
		WinnerData lastWinnerData;                            // last winners snapshot
		NextEpochData nextEpochData;                          // queued config (ticket price)
		Array<PlayerData, QTF_MAX_NUMBER_OF_PLAYERS> players; // current epoch tickets
		id teamAddress;                                       // Dev/team payout address
		id ownerAddress;                                      // config authority
		uint64 numberOfPlayers;                               // tickets count in epoch
		uint64 ticketPrice;                                   // active ticket price
		uint64 jackpot;                                       // jackpot balance
		uint64 targetJackpot;                                 // FR target jackpot
		uint64 overflowAlphaBP;                               // baseline reserve share of overflow (bp)
		uint8 schedule;                                       // bitmask of draw days
		uint8 drawHour;                                       // draw hour UTC
		uint32 lastDrawDateStamp;                             // guard to avoid multiple draws per day
		bit frActive;                                         // FR flag
		uint16 frRoundsSinceK4;                               // rounds since last jackpot hit
		uint16 frRoundsAtOrAboveTarget;                       // hysteresis counter for FR off
		uint8 currentState;                                   // bitmask of STATE_* flags (e.g., STATE_SELLING)
		Array<Array<uint8, QTF_RANDOM_VALUES_COUNT>, QTF_WINNING_COMBINATIONS_HISTORY_SIZE>
		    winningCombinationsHistory;  // ring buffer of winning combinations
		uint64 winningCombinationsCount; // next write position in ring buffer
	};
#@ 2495-2495
};
#@@ file=contracts/QDuel.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 3-4
constexpr uint16 QDUEL_MAX_NUMBER_OF_ROOMS = 512;
constexpr uint64 QDUEL_MAX_NUMBER_OF_WINNER = 128;
#@ 19-28
struct QDUEL : public ContractBase
{
public:
	enum class EState : uint8
	{
		NONE = 0,
		WAIT_TIME = 1 << 0,

		LOCKED = WAIT_TIME
	};
#@ 39-86
	struct RoomInfo
	{
		id roomId;
		id owner;
		id allowedPlayer; // If zero, anyone can join
		sint64 amount;
		uint64 closeTimer;
		DateAndTime lastUpdate;
	};

	struct UserData
	{
		id userId;
		id roomId;
		id allowedPlayer;
		sint64 depositedAmount;
		sint64 locked;
		sint64 stake;
		sint64 raiseStep;
		sint64 maxStake;
	};

	struct WinnerData
	{
		id player1;
		id player2;
		id winner;
		uint64 revenue;

		bool isValid() { return !isZero(player1) && !isZero(player2) && !isZero(winner) && revenue > 0; }
	};

	struct StateData
	{
		HashMap<id, RoomInfo, QDUEL_MAX_NUMBER_OF_ROOMS> rooms;
		HashMap<id, UserData, QDUEL_MAX_NUMBER_OF_ROOMS> users;
		id teamAddress;
		sint64 minimumDuelAmount;
		uint8 devFeePercentBps;
		uint8 burnFeePercentBps;
		uint8 shareholdersFeePercentBps;
		uint8 ttlHours;
		uint8 firstTick;
		EState currentState;
		uint16 percentScale;
		Array<WinnerData, QDUEL_MAX_NUMBER_OF_WINNER> lastWinners;
		uint64 winnerCounter;
	};
#@ 1342-1342
};
#@@ file=contracts/Pulse.h versions=v1.303.2,HEAD
#@ 13-13
using namespace QPI;
#@ 15-24
constexpr uint16 PULSE_MAX_NUMBER_OF_PLAYERS = 1024;
constexpr uint16 PULSE_MAX_NUMBER_OF_AUTO_PARTICIPANTS = div<uint16>(PULSE_MAX_NUMBER_OF_PLAYERS, 2);
constexpr uint8 PULSE_PLAYER_DIGITS = 6;
constexpr uint8 PULSE_PLAYER_DIGITS_ALIGNED = PULSE_PLAYER_DIGITS + 2;
constexpr uint8 PULSE_WINNING_DIGITS = PULSE_PLAYER_DIGITS;
constexpr uint8 PULSE_WINNING_DIGITS_ALIGNED = PULSE_PLAYER_DIGITS_ALIGNED;
constexpr uint8 PULSE_MAX_DIGIT = 9;
constexpr uint8 PULSE_MAX_DIGIT_ALIGNED = PULSE_MAX_DIGIT + 7;
constexpr uint64 PULSE_TICKET_PRICE_DEFAULT = 200000ULL;
constexpr uint16 PULSE_MAX_NUMBER_OF_WINNERS_IN_HISTORY = 1024;
#@ 49-51
struct PULSE : public ContractBase
{
public:
#@ 74-78
	enum class EState : uint8
	{
		SELLING = 1 << 0,
		AUTO_PENDING = 1 << 1,
	};
#@ 104-221
	struct Ticket
	{
		id player;
		Array<uint8, PULSE_PLAYER_DIGITS_ALIGNED> digits;

		bool isValid() const { return !isZero(player); }
	};

	struct AutoParticipant
	{
		id player;
		sint64 deposit;
		uint16 desiredTickets;
	};

	// Forward declaration for use by NextEpochData::apply.
	struct StateData;

	// Deferred settings applied at END_EPOCH to avoid mid-round changes.
	struct NextEpochData
	{
		void clear()
		{
			hasNewPrice = false;
			hasNewSchedule = false;
			hasNewDrawHour = false;
			hasNewFee = false;
			hasNewQHeartHoldLimit = false;
			newPrice = 0;
			newSchedule = 0;
			newDrawHour = 0;
			newDevPercent = 0;
			newBurnPercent = 0;
			newShareholdersPercent = 0;
			newQHeartHoldLimit = 0;
		}

		void apply(StateData& s) const
		{
			if (hasNewPrice)
			{
				s.ticketPrice = newPrice;
			}
			if (hasNewSchedule)
			{
				s.schedule = newSchedule;
			}
			if (hasNewDrawHour)
			{
				s.drawHour = newDrawHour;
			}
			if (hasNewFee)
			{
				s.devPercent = newDevPercent;
				s.burnPercent = newBurnPercent;
				s.shareholdersPercent = newShareholdersPercent;
				s.rlShareholdersPercent = newRLShareholdersPercent;
			}
			if (hasNewQHeartHoldLimit)
			{
				s.qheartHoldLimit = newQHeartHoldLimit;
			}
		}

		bit hasNewPrice;
		bit hasNewSchedule;
		bit hasNewDrawHour;
		bit hasNewFee;
		bit hasNewQHeartHoldLimit;
		uint64 newPrice;
		uint8 newSchedule;
		uint8 newDrawHour;
		uint8 newDevPercent;
		uint8 newBurnPercent;
		uint8 newShareholdersPercent;
		uint8 newRLShareholdersPercent;
		uint64 newQHeartHoldLimit;
	};

	// Winner history entry returned by GetWinners.
	struct WinnerInfo
	{
		id winnerAddress;
		uint64 revenue;
		uint16 epoch;
	};

	struct StateData
	{
		// Ring buffer of recent winners; index is winnersCounter % capacity.
		Array<WinnerInfo, PULSE_MAX_NUMBER_OF_WINNERS_IN_HISTORY> winners;
		// Tickets for the current round; valid range is [0, ticketCounter).
		Array<Ticket, PULSE_MAX_NUMBER_OF_PLAYERS> tickets;
		// Auto-buy participants keyed by user id.
		HashMap<id, AutoParticipant, PULSE_MAX_NUMBER_OF_AUTO_PARTICIPANTS> autoParticipants;
		// Last settled winning digits; undefined before the first draw.
		Array<uint8, PULSE_WINNING_DIGITS_ALIGNED> lastWinningDigits;
		NextEpochData nextEpochData;
		id teamAddress;
		id qheartIssuer;
		// Monotonic winner count used to rotate the winners ring buffer.
		uint64 winnersCounter;
		sint64 ticketCounter;
		sint64 ticketPrice;
		// Contract balance above this cap is swept to the QHeart wallet after settlement.
		uint64 qheartHoldLimit;
		// Date stamp of the most recent draw; PULSE_DEFAULT_INIT_TIME is a bootstrap sentinel.
		uint32 lastDrawDateStamp;
		// Per-user auto-purchase limits; 0 means unlimited.
		uint16 maxAutoTicketsPerUser;
		uint8 devPercent;
		uint8 burnPercent;
		uint8 shareholdersPercent;
		uint8 rlShareholdersPercent;
		uint8 schedule;
		uint8 drawHour;
		EState currentState;
	};
#@ 2279-2279
};
#@@ file=contracts/VottunBridge.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 7-23
struct VOTTUNBRIDGE : public ContractBase
{
public:
    // Bridge Order Structure
    struct BridgeOrder
    {
        id qubicSender;              // Sender address on Qubic
        id qubicDestination;         // Destination address on Qubic
        Array<uint8, 64> ethAddress; // Destination Ethereum address
        uint64 orderId;              // Unique ID for the order
        uint64 amount;               // Amount to transfer
        uint8 orderType;             // Type of order (e.g., mint, transfer)
        uint8 status;                // Order status (e.g., Created, Pending, Refunded)
        bit fromQubicToEthereum;     // Direction of transfer
        bit tokensReceived;          // Flag to indicate if tokens have been received
        bit tokensLocked;            // Flag to indicate if tokens are in locked state
    };
#@ 216-255
    struct AdminProposal
    {
        uint64 proposalId;
        uint8 proposalType;           // Type from ProposalType enum
        id targetAddress;             // For setAdmin/addManager/removeManager (new admin address)
        id oldAddress;                // For setAdmin: which admin to replace
        uint64 amount;                // For withdrawFees or changeThreshold
        Array<id, 16> approvals;      // Array of owner IDs who approved
        uint8 approvalsCount;         // Count of approvals
        bit executed;                 // Whether proposal was executed
        bit active;                   // Whether proposal is active (not cancelled)
    };

public:
    // Contract State
    struct StateData
    {
        Array<BridgeOrder, 1024> orders;
        id feeRecipient;                 // Specific wallet to receive fees
        Array<id, 16> managers;          // Managers list
        uint64 nextOrderId;              // Counter for order IDs
        uint64 lockedTokens;             // Total locked tokens in the contract (balance)
        uint64 totalReceivedTokens;      // Total tokens received
        uint32 sourceChain;              // Source chain identifier (e.g., Ethereum=1, Qubic=0)
        uint32 _tradeFeeBillionths;      // Trade fee in billionths (e.g., 0.5% = 5,000,000)
        uint64 _earnedFees;              // Accumulated fees from trades
        uint64 _distributedFees;         // Fees already distributed to shareholders
        uint64 _earnedFeesQubic;         // Accumulated fees from Qubic trades
        uint64 _distributedFeesQubic;    // Fees already distributed to Qubic shareholders
        uint64 _reservedFees;            // Fees reserved for pending orders (not distributed yet)
        uint64 _reservedFeesQubic;       // Qubic fees reserved for pending orders (not distributed yet)
        uint64 minimumOrderAmount;       // Minimum order amount to prevent zero-fee spam

        // Multisig state
        Array<id, 16> admins;            // List of multisig admins
        uint8 numberOfAdmins;            // Number of active admins
        uint8 requiredApprovals;         // Threshold: number of approvals needed (2 of 3)
        Array<AdminProposal, 32> proposals; // Pending admin proposals
        uint64 nextProposalId;           // Counter for proposal IDs
    };
#@ 2097-2097
};
#@@ file=contracts/Qusino.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 3-6
constexpr uint64 QUSINO_MAX_USERS = 131072;
constexpr uint64 QUSINO_MAX_NUMBER_OF_GAMES = 131072;
constexpr uint64 QUSINO_GAME_SUBMIT_FEE = 100000000;
constexpr uint32 QUSINO_MAX_NUMBER_OF_GAMES_FOR_VOTING_PER_USER = 64;
#@ 74-76
struct QUSINO : public ContractBase
{
public:
#@ 150-157
    struct GameInfo
    {
        Array<uint8, 64> URI;
        id proposer;
        uint32 yesVotes;
        uint32 noVotes;
        uint32 proposedEpoch;
    };
#@ 211-258
    struct STARAndQSC
    {
        uint64 volumeOfSTAR;
        uint64 volumeOfQSC;
    };
    struct EarnedQSCInfo
    {
        id proposer;
        uint32 epoch;
        bool operator==(const EarnedQSCInfo& other) const
        {
            return proposer == other.proposer && epoch == other.epoch;
        }
    };
    struct VoteInfo
    {
        id voter;
        uint64 gameIndex;

        bool operator==(const VoteInfo& other) const
        {
            return voter == other.voter && gameIndex == other.gameIndex;
        }
    };
    //----------------------------------------------------------------------------
    // Define state
    struct StateData
    {
        HashMap<id, STARAndQSC, QUSINO_MAX_USERS> userAssetVolume;
        HashMap<uint64, GameInfo, QUSINO_MAX_NUMBER_OF_GAMES> gameList;
        HashMap<uint64, GameInfo, 1024> failedGameList;
        HashMap<VoteInfo, uint8, QUSINO_MAX_USERS * QUSINO_MAX_NUMBER_OF_GAMES_FOR_VOTING_PER_USER> voteList;
        HashMap<id, uint32, QUSINO_MAX_USERS> userDailyClaimedBonus;
        HashMap<EarnedQSCInfo, uint64, QUSINO_MAX_NUMBER_OF_GAMES> userEarnedQSCInfo;
        id LPDividendsAddress;
        id CCFDividendsAddress;
        id treasuryAddress;
        id QSTIssuer;
        uint64 QSCCirclatingSupply;
        uint64 STARCirclatingSupply;
        uint64 burntSTAR;
        uint64 epochRevenue;
        uint64 maxGameIndex;
        uint64 QSTAssetName;
        uint64 bonusAmount;
        sint64 transferRightsFee;
        uint32 lastClaimedTime;
    };
#@ 1021-1021
};
#@@ file=contracts/Escrow.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 3-7
constexpr uint64 ESCROW_INITIAL_MAX_DEALS = 262144ULL;
constexpr uint64 ESCROW_MAX_DEALS = ESCROW_INITIAL_MAX_DEALS * X_MULTIPLIER;
constexpr uint64 ESCROW_MAX_DEALS_PER_USER = 8;
constexpr uint64 ESCROW_MAX_ASSETS_IN_DEAL = 4;
constexpr uint64 ESCROW_MAX_RESERVED_ASSETS = ESCROW_MAX_DEALS * ESCROW_MAX_ASSETS_IN_DEAL;
#@ 22-64
struct ESCROW : public ContractBase
{
    struct AssetWithAmount
    {
        id issuer;
        uint64 name;
        uint64 amount;
    };

    struct Deal
    {
        sint64 index;
        id acceptorId;
        uint64 offeredQU;
        uint64 offeredAssetsNumber;
        Array<AssetWithAmount, ESCROW_MAX_ASSETS_IN_DEAL> offeredAssets;
        uint64 requestedQU;
        uint64 requestedAssetsNumber;
        Array<AssetWithAmount, ESCROW_MAX_ASSETS_IN_DEAL> requestedAssets;
        uint16 creationEpoch;
    };

    struct EscrowAsset
    {
        id issuer;
        uint64 assetName;

        bool operator==(const EscrowAsset other) const
        {
            return issuer == other.issuer && assetName == other.assetName;
        }

        bool operator!=(const EscrowAsset other) const
        {
            return issuer != other.issuer || assetName != other.assetName;
        }

        inline void setFrom(const Asset& asset)
        {
            issuer = asset.issuer;
            assetName = asset.assetName;
        }
    };
#@ 141-172
    struct _NumberOfReservedShares_input
    {
        id owner;
        id issuer;
        uint64 assetName;
    };

    struct _NumberOfReservedShares_output
    {
        sint64 amount;
    };

    struct StateData
    {
        uint64 _earnedAmount;
        uint64 _distributedAmount;
        HashSet<EscrowAsset, ESCROW_MAX_RESERVED_ASSETS> _earnedTokens;

        sint64 _currentDealIndex;
        HashMap<sint64, Deal, ESCROW_MAX_DEALS> _deals;
        Collection<sint64, ESCROW_MAX_DEALS> _acceptorDealIndexes;
        Collection<sint64, ESCROW_MAX_DEALS> _ownerDealIndexes;
        HashMap<sint64, id, ESCROW_MAX_DEALS> _dealIndexOwnerMap;
        HashSet<id, ESCROW_MAX_DEALS> _ownersSet;
        Collection<AssetWithAmount, ESCROW_MAX_RESERVED_ASSETS> _reservedAssets;

        id _devAddress;

        _NumberOfReservedShares_input _numberOfReservedShares_input;

        _NumberOfReservedShares_output _numberOfReservedShares_output;
    };
#@ 1110-1110
};
#@@ file=contracts/GGWP.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 27-29
constexpr uint64 WOLFPACK_MAX_HOLDERS = 16384;
constexpr uint64 WOLFPACK_MAX_SHAREHOLDERS = 1024; // HashMap capacity must be 2^N and >= 676 SC shares (676 is not a power of two)
constexpr uint64 WOLFPACK_MAX_CLAN_MEMBERS = 8192;
#@ 55-55
constexpr uint64 WOLFPACK_MAX_GOV_PROPOSALS = 8;   // up to this many shareholder proposals active at once
#@ 112-183
struct WOLFPACK : public ContractBase
{
    // A single shareholder governance proposal (one slot in govProposals).
    struct WolfpackGovProposal
    {
        id proposedAddress;     // candidate new admin/reinvest address
        uint64 proposalId;      // unique increasing id (0 = none); votes reference this id
        uint64 proposalEpoch;   // epoch the proposal was opened (for expiry)
        uint8 status;           // 0 = inactive/empty, 1 = active
        uint8 targetType;       // WOLFPACK_GOV_TARGET_ADMIN / _REINVEST
    };

    // ======================== STATE ========================
    struct StateData
    {
        id adminAddress;

        // GGWP token asset reference (external token on QX)
        Asset wpToken;

        // Token holder snapshot (taken at BEGIN_EPOCH) - 70% pool
        HashMap<id, uint64, WOLFPACK_MAX_HOLDERS> holderBalances;
        uint64 totalTokensSnapshot;
        uint64 holderCount;

        // SC shareholders (the 676 IPO shares) are paid via qpi.distributeDividends()
        // and their voting power is queried live via qpi.numberOfShares() - no snapshot kept.

        // Clan system
        HashMap<id, uint64, WOLFPACK_MAX_CLAN_MEMBERS> clanRanks;
        uint64 clanMemberCount;
        uint64 clanWeightedTotal;

        // Revenue tracking
        uint64 pendingRevenue;
        uint64 reinvestmentFund;  // cumulative total sent to reinvestAddress
        uint64 execReserveFund;   // cumulative QU retained in-contract for execution-fee reserve
        uint64 totalDistributed;
        uint64 totalDeposited;
        uint64 lastDistributionEpoch;
        uint64 lastPayoutTick;

        // Exclude addresses from distribution
        id excludeAddress1;
        id excludeAddress2;

        // Recipient of the reinvestment share (10% of each payout)
        id reinvestAddress;

        // Shareholder governance (change adminAddress / reinvestAddress by >51% of SC shares).
        // Multiple proposals can be active at once; each holds one slot.
        Array<WolfpackGovProposal, WOLFPACK_MAX_GOV_PROPOSALS> govProposals;
        uint64 govNextProposalId;   // monotonic counter; assigns a unique id to each proposal
        HashMap<id, uint64, WOLFPACK_MAX_SHAREHOLDERS> govVoteMap; // shareholder -> proposalId they support (one vote)

        // Staking system
        HashMap<id, uint64, WOLFPACK_MAX_HOLDERS> stakedBalances;
        uint64 totalStaked;
        uint64 stakerCount;

        // Unstake requests
        HashMap<id, uint64, WOLFPACK_MAX_HOLDERS> unstakeAmounts;
        HashMap<id, uint64, WOLFPACK_MAX_HOLDERS> unstakeEpochs;
        uint64 unstakeCount;

        // Staking reward pool (GGWP tokens held by SC for distribution)
        uint64 stakingRewardPool;
        uint64 totalStakingRewardsDistributed;

        // Pending (unclaimed) staking rewards per user
        HashMap<id, uint64, WOLFPACK_MAX_HOLDERS> pendingStakingRewards;
    };
#@ 1323-1323
};
#@@ file=contracts/QPayhub.h versions=HEAD
#@ 1-1
using namespace QPI;
#@ 208-208
constexpr uint64 QPAYHUB_RECEIPT_CAPACITY = 262144; // 2^18 (was 65536)
#@ 254-254
constexpr uint64 QPAYHUB_PROMO_CAPACITY = 32; // concurrent promo sellers; grow via redeploy, not a live concern at launch scale
#@ 260-260
constexpr uint64 QPAYHUB_AFFILIATE_CAPACITY = 128; // concurrent active affiliate links; grow via redeploy
#@ 310-323
struct QPAYHUB : public ContractBase
{
    struct Receipt
    {
        id payer;
        id seller;
        id resourceId;
        uint64 nonce;
        sint64 amountPaid;
        sint64 fee;
        uint32 epochPaid;
        uint32 tickPaid;
        bit consumed;
    };
#@ 345-397
    struct Affiliate
    {
        id affiliate;
        uint32 referredAtEpoch;
    };

    struct StateData
    {
        HashMap<id, Receipt, QPAYHUB_RECEIPT_CAPACITY> receipts;
        sint64 feePool;
        uint64 totalPayments;
        uint64 totalVolume;
        uint64 totalFeesCollected;
        uint64 totalFeesDistributed;
        uint64 totalConsumed;
        uint64 totalPurged;

        // ---- ORACLE PRICE FEED ADDITIONS: state ----
        // 1 QUBIC = quUsdNumerator / quUsdDenominator USDT, as of
        // quUsdUpdatedTick. denominator == 0 means no reply has ever
        // landed yet (never trust a fresh contract price before this).
        sint64 quUsdNumerator;
        sint64 quUsdDenominator;
        uint32 quUsdUpdatedTick;
        sint32 priceOracleSubscriptionId; // -1 = not currently subscribed

        // Fixed in INITIALIZE; NULL_ID issuer leaves the 89% in feePool.
        Asset  dividendToken;
        uint64 totalShareholderDividends;
        uint64 totalTokenholderDividends;

        // ---- PROMO RATES ADDITION: state ----
        // operatorId: day-to-day SetPromoRate/RemovePromoRate caller.
        // recoveryId: can ONLY call ChangeOperator - narrow on purpose, see
        // header note above. Both fixed in INITIALIZE to real identities.
        id operatorId;
        id recoveryId;
        HashMap<id, uint64, QPAYHUB_PROMO_CAPACITY> promoFeePermille;

        // ---- AFFILIATE ADDITION: state ----
        // affiliateRegistrarId: day-to-day SetAffiliate/RemoveAffiliate caller,
        // a separate role from operatorId so a compromised key only ever
        // touches one of the two admin surfaces. recoveryId (above) can
        // reassign this one too, via ChangeAffiliateRegistrar.
        id affiliateRegistrarId;
        HashMap<id, Affiliate, QPAYHUB_AFFILIATE_CAPACITY> affiliateOf;

        // ---- BURN ADDITION: state ----
        // Lifetime QU burned back into this contract's execution fee reserve
        // by END_EPOCH. Appended at the end so every field above keeps its
        // offset, same convention the GetInfo output struct follows.
        uint64 totalBurned;
    };
#@ 1497-1497
};
#@@ file=contracts/QTREAT.h versions=HEAD
#@ 1-1
using namespace QPI;
#@ 3-5
constexpr uint64 QTREAT_MAX_HOLDERS      = 131072;
constexpr uint64 QTREAT_MAX_STAKERS      = 65536;
constexpr uint64 QTREAT_MAX_ASSETS       = 1024;
#@ 32-32
constexpr uint64 QTREAT_ASIC_CATALOG_CAPACITY = 512;
#@ 34-34
constexpr uint64 QTREAT_MAX_ASIC_RIGS         = 128;
#@ 70-71
constexpr uint64 QTREAT_MAX_NFT_HOLDERS = 1024;
constexpr uint64 QTREAT_MAX_DIVIDEND_NFTS = 256;
#@ 73-74
constexpr uint64 QTREAT_MAX_EXCLUDE_ADDRESSES = 10;  // usable slots (0..9)
constexpr uint64 QTREAT_EXCLUDE_CAPACITY = 16;      // Array capacity must be a power of 2
#@ 102-103
constexpr uint64 QTREAT_MULTISIG_SIGNERS   = 6;   // admin + 5
constexpr uint64 QTREAT_MULTISIG_CAPACITY  = 8;   // Array capacity (2^N >= signers)
#@ 134-147
struct QTREAT : public ContractBase
{
    struct StakerInfo
    {
        uint64 staked;
        uint64 unstakeAmount;
        uint64 unstakeEpoch;
        uint64 bonusEpochs;
        uint64 hwmHoldings;
        uint64 lastStaked;
        uint64 growthStreak;
        uint64 bonusAwarded;
        uint64 pendingBonus;
    };
#@ 160-260
    struct AsicRig
    {
        id owner;
        uint32 partMotherboard;
        uint32 partChip;
        uint32 partPsu;
        uint32 partFan;
        uint64 weight;
        uint64 active;
    };

    struct AssetKey
    {
        id issuer;
        uint64 assetName;
        bool operator==(const AssetKey& o) const { return issuer == o.issuer && assetName == o.assetName; }
        bool operator!=(const AssetKey& o) const { return issuer != o.issuer || assetName != o.assetName; }
    };

    // A pending admin asset-withdrawal request (see the multisig constants).
    struct PendingRevoke
    {
        Asset asset;
        id destination;        // must be one of the multisig signers
        uint64 amount;
        uint64 proposalId;     // unique per proposal; approvals are bound to it
        uint64 approvalMask;   // bit i set => signer i approved this proposalId
        uint64 approvalCount;
        uint64 active;         // 1 = open
        DateAndTime proposedAt;
        DateAndTime approvedAt; // set when approvals reach the threshold; the 72h clock runs from here
    };

    struct StateData
    {
        id adminAddress;

        Asset qtreatToken;
        Asset qdogeToken;

        uint64 dividendFund;
        uint64 totalDividendsDistributed;

        uint64 stakingFund;
        uint64 totalStakingRewardsDistributed;

        HashMap<id, uint64, QTREAT_MAX_HOLDERS> beginBalances;
        HashMap<id, uint64, QTREAT_MAX_HOLDERS> endBalances;
        uint64 totalHoldersSnapshot;

        HashMap<id, StakerInfo, QTREAT_MAX_STAKERS> stakers;
        uint64 totalStaked;
        uint64 stakingStartEpoch;

        uint64 qtreatBonusPool;

        Array<id, QTREAT_EXCLUDE_CAPACITY> excludeAddresses; // only the first QTREAT_MAX_EXCLUDE_ADDRESSES slots are used

        Array<uint32, QTREAT_MAX_DIVIDEND_NFTS> dividendNftIds;
        uint64 dividendNftIdCount;
        HashMap<id, uint64, QTREAT_MAX_NFT_HOLDERS> nftCounts;
        uint64 totalNftCount;

        uint64 totalShareholderDividends;

        HashMap<uint64, uint64, QTREAT_ASIC_CATALOG_CAPACITY> asicCatalog;
        Array<uint64, 32> asicCatalogBuckets;
        uint64 asicCatalogSize;
        uint64 asicCatalogLocked;
        HashMap<uint64, uint64, QTREAT_ASIC_CATALOG_CAPACITY> asicUsedParts;
        Array<AsicRig, QTREAT_MAX_ASIC_RIGS> asicRigs;
        uint64 asicRigHighWater;
        uint64 totalAsicCount;
        uint64 totalMiningWeight;
        uint64 miningFund;
        uint64 miningRewardRate;
        uint64 totalMiningRewardsDistributed;

        uint64 dripQdogePool;
        uint64 dripStartEpoch;
        uint64 totalDripQdogeDistributed;
        HashMap<id, uint64, QTREAT_ASIC_CATALOG_CAPACITY> dripTally;

        uint64 totalBonusDelivered;

        uint64 totalRaffleAwarded;
        id lastRaffleWinner;
        uint64 lastRaffleEpoch;

        HashMap<AssetKey, uint64, QTREAT_MAX_ASSETS> generalAssetBalances;
        HashMap<id, uint64, QTREAT_MAX_ASSETS> scDividendTracker;

        // ---- Multisig admin asset withdrawal ----
        Array<id, QTREAT_MULTISIG_CAPACITY> multisigSigners; // slot 0 = admin
        PendingRevoke pendingRevoke;
        uint64 multisigNextProposalId;

        // ---- Multisig admin rotation ----
        // adminApprovals[i] = the wallet signer i currently approves as the new admin (NULL_ID = none).
        Array<id, QTREAT_MULTISIG_CAPACITY> adminApprovals;
    };
#@ 2230-2230
};
#@@ file=contracts/TestExampleA.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 3-3
constexpr uint64 TESTEXA_ASSET_NAME = 18392928276923732;
#@ 9-10
struct TESTEXA : public ContractBase
{
#@ 14-105
	struct QpiFunctionsOutput
	{
		id arbitrator;
		id computor0;
		id invocator;
		id originator;
		sint64 invocationReward;
		sint32 numberOfTickTransactions;
		uint32 tick;
		uint16 epoch;
		uint16 millisecond;
		uint8 year;   // [0..99] (0 = 2000, 1 = 2001, ..., 99 = 2099)
		uint8 month;  // [1..12]
		uint8 day;    // [1..31]
		uint8 hour;   // [0..23]
		uint8 minute; // [0..59]
		uint8 second;
		uint8 dayOfWeek;
	};

	// MultiVariables proposal option data type, which is custom per contract
	struct MultiVariablesProposalExtraData
	{
		struct Option
		{
			uint64 dummyStateVariable1;
			uint32 dummyStateVariable2;
			sint8 dummyStateVariable3;
		};

		Option optionYesValues;
		bool hasValueDummyStateVariable1;
		bool hasValueDummyStateVariable2;
		bool hasValueDummyStateVariable3;

		bool isValid() const
		{
			return hasValueDummyStateVariable1 || hasValueDummyStateVariable2 || hasValueDummyStateVariable3;
		}
	};

	// Proposal data type. We only support yes/no voting.
	typedef ProposalDataYesNo ProposalDataT;

	// Shareholders of TESTEXA have right to propose and vote. Only 16 slots provided.
	typedef ProposalAndVotingByShareholders<16, TESTEXA_ASSET_NAME> ProposersAndVotersT;

	// Proposal and voting storage type
	typedef ProposalVoting<ProposersAndVotersT, ProposalDataT> ProposalVotingT;

	//---------------------------------------------------------------
	// State

	struct StateData
	{
		//---------------------------------------------------------------
		// QPI FUNCTION TESTING state fields
		QpiFunctionsOutput qpiFunctionsOutputTemp;
		Array<QpiFunctionsOutput, 16> qpiFunctionsOutputBeginTick; // Output of QPI functions queried by the BEGIN_TICK procedure for the last 16 ticks
		Array<QpiFunctionsOutput, 16> qpiFunctionsOutputEndTick; // Output of QPI functions queried by the END_TICK procedure for the last 16 ticks
		Array<QpiFunctionsOutput, 16> qpiFunctionsOutputUserProc; // Output of QPI functions queried by the USER_PROCEDURE

		//---------------------------------------------------------------
		// ASSET MANAGEMENT RIGHTS TRANSFER state fields
		PreManagementRightsTransfer_output preReleaseSharesOutput;
		PreManagementRightsTransfer_output preAcquireSharesOutput;

		PreManagementRightsTransfer_input prevPreReleaseSharesInput;
		PreManagementRightsTransfer_input prevPreAcquireSharesInput;
		PostManagementRightsTransfer_input prevPostReleaseSharesInput;
		PostManagementRightsTransfer_input prevPostAcquireSharesInput;
		uint32 postReleaseSharesCounter;
		uint32 postAcquireShareCounter;

		//---------------------------------------------------------------
		// CONTRACT INTERACTION / RESOLVING DEADLOCKS / ERROR HANDLING state fields
		sint64 heavyComputationResult;

		//---------------------------------------------------------------
		// SHAREHOLDER PROPOSALS state fields

		// Variables that can be set with proposals
		uint64 dummyStateVariable1;
		uint32 dummyStateVariable2;
		sint8 dummyStateVariable3;

		// Proposal storage
		ProposalVotingT proposals;

		// MultiVariables proposal option data storage (same number of slots as proposals)
		Array<MultiVariablesProposalExtraData, 16> multiVariablesProposalData;
	};
#@ 864-864
};
#@@ file=contracts/TestExampleB.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 3-3
constexpr uint64 TESTEXB_ASSET_NAME = 18674403253634388;
#@ 9-10
struct TESTEXB : public ContractBase
{
#@ 14-64
	struct IncomingTransferAmounts
	{
		sint64 standardTransactionAmount;
		sint64 procedureTransactionAmount;
		sint64 qpiTransferAmount;
		sint64 qpiDistributeDividendsAmount;
		sint64 revenueDonationAmount;
		sint64 ipoBidRefundAmount;
	};

	// Proposal data type. Support up to 8 options and scalar voting.
	typedef ProposalDataV1<true> ProposalDataT;

	// Shareholders of TESTEXB have right to propose and vote. Only 16 slots provided.
	typedef ProposalAndVotingByShareholders<16, TESTEXB_ASSET_NAME> ProposersAndVotersT;

	// Proposal and voting storage type
	typedef ProposalVoting<ProposersAndVotersT, ProposalDataT> ProposalVotingT;

	//---------------------------------------------------------------
	// State

	struct StateData
	{
		//---------------------------------------------------------------
		// ASSET MANAGEMENT RIGHTS TRANSFER state fields
		PreManagementRightsTransfer_output preReleaseSharesOutput;
		PreManagementRightsTransfer_output preAcquireSharesOutput;

		PreManagementRightsTransfer_input prevPreReleaseSharesInput;
		PreManagementRightsTransfer_input prevPreAcquireSharesInput;
		PostManagementRightsTransfer_input prevPostReleaseSharesInput;
		PostManagementRightsTransfer_input prevPostAcquireSharesInput;
		uint32 postReleaseSharesCounter;
		uint32 postAcquireShareCounter;

		//---------------------------------------------------------------
		// POST_INCOMING_TRANSFER CALLBACK state fields
		IncomingTransferAmounts incomingTransfers;

		//---------------------------------------------------------------
		// SHAREHOLDER PROPOSALS state fields

		// Variables that can be set with proposals
		sint64 fee1;
		sint64 fee2;
		sint64 fee3;

		// Proposal storage
		ProposalVotingT proposals;
	};
#@ 637-637
};
#@@ file=contracts/TestExampleC.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 7-8
struct TESTEXC : public ContractBase
{
#@ 12-31
	struct IncomingTransferAmounts
	{
		sint64 standardTransactionAmount;
		sint64 procedureTransactionAmount;
		sint64 qpiTransferAmount;
		sint64 qpiDistributeDividendsAmount;
		sint64 revenueDonationAmount;
		sint64 ipoBidRefundAmount;
	};

	//---------------------------------------------------------------
	// State

	struct StateData
	{
		int waitInPreAcqiuireSharesCallback;
		IncomingTransferAmounts incomingTransfers;
		HashMap<uint64, uint32, 64> oracleQueryExtraData;
		uint32 oracleSubscriptionId;
	};
#@ 488-488
};
#@@ file=contracts/TestExampleD.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 7-8
struct TESTEXD : public ContractBase
{
#@ 29-29
};
#@@ file=contracts/EmptyTemplate.h versions=v1.303.2,HEAD
#@ 1-1
using namespace QPI;
#@ 7-14
struct CNAME : public ContractBase
{
	// All persistent state fields must be declared inside StateData.
	// Access state with state.get().field (read) and state.mut().field (write).
	// state.mut() marks the contract state as dirty for automatic change detection.
	struct StateData
	{
	};
#@ 63-63
};
#@@ file=contract_core/contract_def.h versions=v1.303.2
#@ 370-374
struct IPO
{
    m256i publicKeys[NUMBER_OF_COMPUTORS];
    long long prices[NUMBER_OF_COMPUTORS];
};
#@@ file=contract_core/contract_def.h versions=HEAD
#@ 390-394
struct IPO
{
    m256i publicKeys[NUMBER_OF_COMPUTORS];
    long long prices[NUMBER_OF_COMPUTORS];
};
'''

if __name__ == '__main__':
    sys.exit(main())
