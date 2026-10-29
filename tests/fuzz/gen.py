#!/usr/bin/env python3
"""Random Svalinn program generator for differential testing of the optimizer.
Every generated program is deterministic and prints many values; it must print the same output
when compiled with and without -O (and, with --c, the same as an equivalent C program where defined).
usage: gen.py SEED > prog.svl"""
import random, sys

INT_TYPES = ['i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64']
rnd = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1)

class Ctx:
    def __init__(self):
        self.vars = []      # (name, type, mutable)
        self.n = 0
    def fresh(self, p='v'):
        self.n += 1
        return f'{p}{self.n}'

def lit(t):
    if t in ('f32', 'f64'):
        return rnd.choice(['0.5', '1.25', '2.0', '3.75', '0.1', '100.0', '7.0', '1e3', '0.001'])
    bits = int(t[1:]); signed = t[0] == 'i'
    choices = [0, 1, 2, 3, 7, 13, 100, 255, 1000, 65535, 2**31 - 1, 2**31, 2**32 + 5, 2**62]
    v = rnd.choice(choices)
    lim = (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1
    v = v % (lim + 1)
    return str(v)

def var_of(ctx, t):
    vs = [v for v in ctx.vars if v[1] == t]
    return rnd.choice(vs)[0] if vs else None

def expr(ctx, t, d=0):
    r = rnd.random()
    if d > 3 or r < 0.25:
        v = var_of(ctx, t)
        if v and rnd.random() < 0.7: return v
        return lit(t) if not t.startswith('i') or rnd.random() < 0.8 else f'(0 - {lit(t)})' if t[0] == 'i' else lit(t)
    if t in ('f32', 'f64'):
        op = rnd.choice(['+', '-', '*', '/'])
        if rnd.random() < 0.15:
            st = rnd.choice(INT_TYPES)
            return f'(c_{st}({expr(ctx, st, d + 1)}) as {t})'
        return f'({expr(ctx, t, d + 1)} {op} {expr(ctx, t, d + 1)})'
    ops = ['+', '-', '*', '&', '|', '^', '/', '%', '<<', '>>']
    op = rnd.choice(ops)
    if rnd.random() < 0.1:
        st = rnd.choice(INT_TYPES + ['f64'])
        return f'(c_{st}({expr(ctx, st, d + 1)}) as {t})'
    if rnd.random() < 0.08:
        return f'(if_({cond(ctx, d + 1)}) as {t})' if False else f'(sel_{t}({cond(ctx, d + 1)}, {expr(ctx, t, d + 1)}, {expr(ctx, t, d + 1)}))'
    a = expr(ctx, t, d + 1)
    if op in ('/', '%'):
        return f'({a} {op} nz_{t}({expr(ctx, t, d + 1)}))'
    if op in ('<<', '>>'):
        return f'({a} {op} ({expr(ctx, t, d + 1)} & {int(t[1:]) - 1}))'
    return f'({a} {op} {expr(ctx, t, d + 1)})'

def cond(ctx, d=0):
    r = rnd.random()
    if r < 0.15 and d < 3:
        return f'({cond(ctx, d + 1)} && {cond(ctx, d + 1)})'
    if r < 0.3 and d < 3:
        return f'({cond(ctx, d + 1)} || {cond(ctx, d + 1)})'
    if r < 0.35 and d < 3:
        return f'!{cond(ctx, d + 1)}'
    t = rnd.choice(INT_TYPES + ['f64'])
    op = rnd.choice(['<', '<=', '>', '>=', '==', '!='])
    return f'(c_{t}({expr(ctx, t, d + 2)}) {op} {expr(ctx, t, d + 2)})'

def ilit(v):
    return str(v) if v >= 0 else f'(0 - {-v - 1} - 1)'

def index_expr(ctx):
    lv = [v[0] for v in ctx.vars if v[1] == 'i32' and v[0].startswith('i') and not v[2]]
    a = rnd.choice(lv) if lv else str(rnd.randint(0, 7))
    b = rnd.choice(lv) if lv else str(rnd.randint(0, 7))
    e = rnd.choice([a, f'({a} + {b})', f'({a} * {rnd.randint(1, 3)} + {b})', f'({a} * 2 + {rnd.randint(0, 3)})', f'({a} - 1)'])
    return f'({e} & 7)' if rnd.random() < 0.9 else e

def stmt(ctx, depth, out, ind, in_loop=False):
    r = rnd.random()
    p = '    ' * ind
    if r < 0.3:
        t = rnd.choice(INT_TYPES + ['f64', 'f32'])
        n = ctx.fresh()
        out.append(f'{p}var {n}: {t} = {expr(ctx, t)};')
        ctx.vars.append((n, t, True))
    elif r < 0.5 and [v for v in ctx.vars if v[2]]:
        n, t, m = rnd.choice([v for v in ctx.vars if v[2]])
        if rnd.random() < 0.5 and t not in ('f32', 'f64'):
            op = rnd.choice(['+=', '-=', '*=', '^=', '|=', '&='])
            out.append(f'{p}{n} {op} {expr(ctx, t)};')
        else:
            out.append(f'{p}{n} = {expr(ctx, t)};')
    elif r < 0.62 and depth < 3:
        out.append(f'{p}if {cond(ctx)} {{')
        saved = list(ctx.vars)
        for _ in range(rnd.randint(1, 3)): stmt(ctx, depth + 1, out, ind + 1, in_loop)
        ctx.vars = list(saved)
        if rnd.random() < 0.5:
            out.append(f'{p}}} else {{')
            for _ in range(rnd.randint(1, 3)): stmt(ctx, depth + 1, out, ind + 1, in_loop)
            ctx.vars = list(saved)
        out.append(f'{p}}}')
    elif r < 0.72 and depth < 2:
        i = ctx.fresh('i')
        t = rnd.choice(['i32', 'i64', 'u32', 'u64'])
        hi = str(rnd.randint(0, 6))
        lo = str(rnd.randint(0, 2))
        if rnd.random() < 0.5: hi = f'(c_i32({expr(ctx, "i32")}) & 7)'
        if rnd.random() < 0.3: lo = f'(c_i32({expr(ctx, "i32")}) & 3)'
        out.append(f'{p}for {i} in {lo}..{hi} {{')
        saved = list(ctx.vars)
        ctx.vars.append((i, 'i32', False))
        for _ in range(rnd.randint(1, 3)): stmt(ctx, depth + 1, out, ind + 1, True)
        if rnd.random() < 0.2: out.append(f'{p}    if {cond(ctx)} {{ {rnd.choice(["break", "continue"])}; }}')
        ctx.vars = list(saved)
        out.append(f'{p}}}')
    elif r < 0.76:
        out.append(f'{p}poke(ref arr, {expr(ctx, "u8")}, {expr(ctx, "i64")});')
        out.append(f'{p}var la{ctx.n}: [5]i32;')
        out.append(f'{p}la{ctx.n}[({expr(ctx, "u8")}) % 5] = {expr(ctx, "i32")};')
        out.append(f'{p}println(sum5(ref la{ctx.n}));')
        ctx.n += 1
    elif r < 0.8:
        k = rnd.randint(0, 7)
        out.append(f'{p}arr[{k}] = {expr(ctx, "i64")};')
        out.append(f'{p}println(arr[({expr(ctx, "u8")}) % 8]);')
    elif r < 0.85:
        out.append(f'{p}println(mix({expr(ctx, "i64")}, {expr(ctx, "i32")}, {expr(ctx, "f64")}));')
    elif r < 0.88:
        t = rnd.choice(INT_TYPES)
        n = ctx.fresh('k')
        out.append(f'{p}let {n}: {t} = {lit(t)};')
        ctx.vars.append((n, t, False))
    elif r < 0.93:
        ix = index_expr(ctx)
        k = rnd.random()
        if k < 0.3: out.append(f'{p}farr[{ix}] {rnd.choice(["+=", "-=", "*=", "="])} {expr(ctx, "f64")};')
        elif k < 0.5: out.append(f'{p}sarr[{ix} & 3].{rnd.choice(["b", "c"])} {rnd.choice(["+=", "-=", "="])} {expr(ctx, "f64")};')
        elif k < 0.7: out.append(f'{p}sarr[{ix} & 3].a {rnd.choice(["+=", "-=", "^=", "="])} {expr(ctx, "i64")};')
        elif k < 0.78: out.append(f'{p}arr[{ix}] {rnd.choice(["+=", "-=", "|=", "="])} lin({expr(ctx, "i64")}, getr(ref arr, {expr(ctx, "i64")}));')
        elif k < 0.85:
            if rnd.random() < 0.5: out.append(f'{p}sarr[{ix} & 3] = P {{ c: {expr(ctx, "f64")}, a: {expr(ctx, "i64")}, b: {expr(ctx, "f64")} }};')
            else: out.append(f'{p}println(mkp({expr(ctx, "i64")}, {expr(ctx, "f64")}).{rnd.choice(["a", "b", "c"])});')
        else: out.append(f'{p}println(sq(farr[{ix}]) + farr[{index_expr(ctx)}] * sarr[{ix} & 3].c);')
    elif r < 0.95 and rnd.random() < 0.5 or rnd.random() < 0.1:
        # vectorizable element-wise loops (and near misses)
        q = ctx.fresh('q'); t = rnd.choice(['f64', 'f64', 'f32'])
        a, b = ('farr', 'farr2') if t == 'f64' else ('garr', 'garr2')
        lo = rnd.randint(0, 4); hi = rnd.choice([rnd.randint(0, 12), f'(c_i64({expr(ctx, "i64")}) & 15)'])
        off = rnd.choice(['0', '1', '2', f'(c_i64({expr(ctx, "i64")}) & 7)', '(0 - 1)'])
        fv = [v[0] for v in ctx.vars if v[1] == t]
        sc = rnd.choice(fv) if fv and rnd.random() < 0.6 else lit(t)
        rhs = rnd.choice([f'{sc} * {b}[{q}]', f'{b}[{q} + {off}] - {sc}', f'({b}[{q}] + {a}[{q} + {off}]) * {lit(t)}',
                          f'{b}[{q}] / ({sc} + {lit(t)})', f'{sc}', f'{a}[{q} + {off}] * {b}[{q}]', f'({q} as {t}) * {sc}'])
        op = rnd.choice(['=', '+=', '-=', '*='])
        out.append(f'{p}for {q} in {lo} as i64..{hi} as i64 {{ {a}[{q} + {off}] {op} {rhs}; }}')
    elif r < 0.96 and depth < 2:
        # counting while loops near the type limits (normalisation after += / -=)
        t = rnd.choice(['i8', 'u8', 'i16', 'u16', 'i32', 'u32'])
        bits = int(t[1:]); sg = t[0] == 'i'
        mx = (1 << (bits - 1)) - 1 if sg else (1 << bits) - 1; mn = -(1 << (bits - 1)) if sg else 0
        w = ctx.fresh('w'); w2 = ctx.fresh('w')
        up = rnd.random() < 0.5; c = rnd.choice([1, 1, 2, 3])
        if up:
            start = mx - rnd.randint(0, 8); lim = rnd.choice([str(mx), str(mx - 1), w2])
            wc = f'{w} {rnd.choice(["<", "<="])} {lim}'; step = f'{w} += {c};'
        else:
            start = mn + rnd.randint(0, 8); lim = rnd.choice([ilit(mn), ilit(mn + 1), w2])
            wc = f'{rnd.choice([lim + " < " + w, w + " > " + lim])}'; step = f'{w} -= {c};'
        out.append(f'{p}var {w}: {t} = {ilit(start)};')
        out.append(f'{p}var {w2}: {t} = {ilit(mx if up else mn)};')
        out.append(f'{p}var {ctx.fresh("n")}: i32 = 0;')
        cnt = f'n{ctx.n}'
        out.append(f'{p}while {wc} && {cnt} < 20 {{ {cnt} += 1; {step} println({w}); }}')
        out.append(f'{p}println({w});')
    elif r < 0.97 and depth < 2 and in_loop:
        lv = [v for v in ctx.vars if v[0].startswith('i') and not v[2]]
        if lv:
            i = ctx.fresh('i')
            out.append(f'{p}for {i} in {rnd.choice(lv)[0]} + 1..{rnd.randint(1, 8)} {{')
            saved = list(ctx.vars)
            ctx.vars.append((i, 'i32', False))
            for _ in range(rnd.randint(1, 3)): stmt(ctx, depth + 1, out, ind + 1, True)
            ctx.vars = list(saved)
            out.append(f'{p}}}')
    else:
        if ctx.vars:
            n, t, m = rnd.choice(ctx.vars)
            out.append(f'{p}println({n});')

def main():
    out = ['module Fuzz;', 'allow Console;', '', 'var arr: [8]i64;', 'var farr: [8]f64;', 'var farr2: [12]f64;', 'var garr: [8]f32;', 'var garr2: [12]f32;', 'struct P { a: i64; b: f64; c: f64; }', 'var sarr: [4]P;', '',
           'fn sq(x: f64) -> f64 { return x * x; }', 'fn lin(a: i64, b: i64) -> i64 { return a * 3 + b; }',
           'fn getr(ref a: [8]i64, i: i64) -> i64 { return a[i & 7]; }',
           'fn mkp(a: i64, b: f64) -> P { return P { a, b, c: b * 2.0 }; }']
    for t in INT_TYPES + ['f64', 'f32']:
        out.append(f'fn c_{t}(x: {t}) -> {t} {{ return x; }}')
    for t in INT_TYPES:
        out.append(f'fn nz_{t}(x: {t}) -> {t} {{ if x == 0 {{ return 1; }} return x; }}')
        out.append(f'fn sel_{t}(c: bool, a: {t}, b: {t}) -> {t} {{ if c {{ return a; }} return b; }}')
    out.append('fn poke(ref a: [8]i64, i: u8, v: i64) { a[i % 8] = a[(i + 1) % 8] + v; a[(i as i64 * 3) % 8] -= 1; }')
    out.append('fn sum5(ref a: [5]i32) -> i64 { var s: i64 = 0; for i in 0..5 { s += a[i] as i64 * (i as i64 + 1); } return s; }')
    out.append('fn mix(a: i64, b: i32, c: f64) -> i64 { var r = a * 3 + (b as i64); if c > 10.0 { r -= 1; } return r ^ (r >> 7); }')
    out.append('fn initv() { for q in 0..12 { farr2[q] = (q as f64) * 0.75 - 2.0; garr2[q] = (q as f32) * 1.5 + 0.25; } }')
    out.append('fn main() -> i32 {')
    ctx = Ctx()
    body = []
    for _ in range(rnd.randint(15, 40)):
        stmt(ctx, 0, body, 1)
    for n, t, m in ctx.vars:
        body.append(f'    println({n});')
    body.append('    for q in 0..8 { println(farr[q]); println(arr[q]); println(garr[q]); }')
    body.insert(0, '    initv();')
    body.append('    for q in 0..4 { println(sarr[q].a); println(sarr[q].b); println(sarr[q].c); }')
    out += body
    out.append('    return 0;')
    out.append('}')
    print('\n'.join(out))

main()
