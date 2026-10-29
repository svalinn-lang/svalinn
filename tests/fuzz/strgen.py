#!/usr/bin/env python3
"""Random String programs with a Python oracle: prints an Aegis program to stdout and its expected output to
the file given as second argument. Exercises concatenation, slicing, methods, comparisons, copies through
variables / struct fields / arrays / Vec / calls, so reference counting mistakes show up as wrong output
or (under valgrind) as leaks and invalid accesses.
usage: strgen.py SEED expected.txt > prog.ag"""
import random, sys

rnd = random.Random(int(sys.argv[1]))
LITS = ["", "a", "ab", "abc", " x ", "Hello", "zz", "  pad\t", "a-b-c", "xyzzy", "AbC"]
NV = 5
vals = [""] * NV          # oracle values of s0..s4
arr = [""] * 3            # arr: [3]String
box = [""]                # box.s
vec = []                  # Vec<String>
out = []
body = []

def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\n', '\\n') + '"'
def emit(line): body.append('    ' + line)
def b(x): return 'true' if x else 'false'
def pick(): return rnd.randrange(NV)
def keep_short(i):
    if len(vals[i]) > 120:
        emit(f's{i} = s{i}.slice(0, 40);'); vals[i] = vals[i][:40]

for step in range(rnd.randint(30, 90)):
    r = rnd.random(); x, y, z = pick(), pick(), pick()
    if r < 0.10: emit(f's{x} = {q(l := rnd.choice(LITS))};'); vals[x] = l
    elif r < 0.20: emit(f's{x} = s{y} + s{z};'); vals[x] = vals[y] + vals[z]
    elif r < 0.25:
        n = rnd.choice([0, 7, -3, 42]); emit(f's{x} = s{y} + {n} + {q("|")};'); vals[x] = vals[y] + str(n) + "|"
    elif r < 0.30:
        n = len(vals[y]); a = rnd.randint(0, n); c = rnd.randint(a, n)
        emit(f's{x} = s{y}.slice({a}, {c});'); vals[x] = vals[y][a:c]
    elif r < 0.34: emit(f's{x} = s{y}.to_upper();'); vals[x] = vals[y].upper()
    elif r < 0.37: emit(f's{x} = s{y}.to_lower();'); vals[x] = vals[y].lower()
    elif r < 0.40: emit(f's{x} = s{y}.trim();'); vals[x] = vals[y].strip(' \t\n\r\x0b\x0c')
    elif r < 0.43:
        f, t = rnd.choice(["a", "b", "-", "z", "ab"]), rnd.choice(["", "Q", "--", "abc"])
        emit(f's{x} = s{y}.replace({q(f)}, {q(t)});'); vals[x] = vals[y].replace(f, t)
    elif r < 0.45:
        k = rnd.randint(0, 3); emit(f's{x} = s{y}.repeat({k});'); vals[x] = vals[y] * k
    elif r < 0.50: emit(f'println(s{x});'); out.append(vals[x])
    elif r < 0.54: emit(f'println(s{x}.len());'); out.append(str(len(vals[x])))
    elif r < 0.58:
        l = rnd.choice(LITS[1:]); emit(f'println(s{x}.find({q(l)}));'); out.append(str(vals[x].find(l)))
    elif r < 0.62: emit(f'println(s{x} == s{y});'); out.append(b(vals[x] == vals[y]))
    elif r < 0.65: emit(f'println(s{x} < s{y});'); out.append(b(vals[x] < vals[y]))
    elif r < 0.68:
        l = rnd.choice(LITS); emit(f'println(s{x}.starts_with({q(l)}) || s{x}.ends_with({q(l)}));')
        out.append(b(vals[x].startswith(l) or vals[x].endswith(l)))
    elif r < 0.72: i = rnd.randrange(3); emit(f'arr[{i}] = s{x};'); arr[i] = vals[x]
    elif r < 0.76: i = rnd.randrange(3); emit(f's{x} = arr[{i}] + "";'); vals[x] = arr[i]
    elif r < 0.79: emit(f'bx.s = s{x};'); box[0] = vals[x]
    elif r < 0.82: emit(f'let cp{step} = bx; s{x} = cp{step}.s;'); vals[x] = box[0]
    elif r < 0.85: emit(f's{x} = join2(s{y}, s{z});'); vals[x] = vals[z] + "/" + vals[y]
    elif r < 0.87: emit(f's{x} = first5(s{y});'); vals[x] = vals[y][:5] if len(vals[y]) > 5 else vals[y]
    elif r < 0.90:
        emit(f'n = 0; for c in s{x} {{ if c == {ord("a")} {{ n += 1; }} }} println(n);'); out.append(str(vals[x].count("a")))
    elif r < 0.94: emit(f'v.push(s{x});'); vec.append(vals[x])
    elif r < 0.97 and vec:
        i = rnd.randrange(len(vec)); emit(f's{x} = v.item({i});'); vals[x] = vec[i]
    else:
        emit(f'println(str(v.len()) + ":" + s{x}.contains("a"));'); out.append(f'{len(vec)}:{b("a" in vals[x])}')
    keep_short(x)
for i in range(NV): emit(f'println(s{i});'); out.append(vals[i])
for i in range(3): emit(f'println(arr[{i}]);'); out.append(arr[i])
emit('for e in v { print(e); print(","); } println("");'); out.append("".join(e + "," for e in vec))

print("module StrFuzz;\nallow Console;\nimport std.vec;\n")
print("struct Box { s: String; n: i32; }\n")
print("fn join2(a: String, b: String) -> String { return b + \"/\" + a; }")
print("fn first5(a: String) -> String { if a.len() > 5 { return a.slice(0, 5); } return a; }\n")
print("fn main() -> i32 {")
for i in range(NV): print(f'    var s{i} = "";')
print("    var arr: [3]String;\n    var bx = Box { s: \"\", n: 1 };\n    var v = Vec<String>();\n    var n = 0;")
print("\n".join(body))
print("    return 0;\n}")
open(sys.argv[2], "w").write("\n".join(out) + "\n")
