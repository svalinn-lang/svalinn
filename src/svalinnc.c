/*
 * svalinnc - the Svalinn compiler (bootstrap implementation in C; selfhost/ holds the same compiler written in Svalinn)
 * Target: x86-64 System V (Linux), emits GNU as (Intel syntax), links with gcc.
 * Pipeline: lex -> parse (AST) -> sema (types, safety, capabilities) -> codegen
 *
 * Language: modules and imports, capabilities (allow/deny/requires) and protect Process, functions, structs,
 * classes with inheritance and interfaces, generics, enums, Result and '?', ownership with destructors,
 * reference-counted Strings, fixed arrays (bounds-checked), match, loops, async tasks, unsafe code and C calls.
 * -O enables the optimizer (register allocation, inlining, constant folding, bounds-check elimination).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <setjmp.h>
#define SVALINN_VERSION "1.0.0-rc.1"

/* ============================ util ============================ */
static const char *g_path = "<input>";
static char **files; static int nfiles, cfiles;
static int rt_fidx = -1;   /* file index of lib/rt/prelude.svl */
static char file_trusted[32];   /* files of the runtime and the standard library: no capability checks inside
                                   * (their entry points declare `requires` instead) */
/* source positions: (file index << 26) | (column << 16) | line; column 0 = unknown (no caret is shown) */
#define POS_FILE(p) ((p) >> 26)
#define POS_COL(p) (((p) >> 16) & 1023)
#define POS_LINE(p) ((p) & 0xFFFF)
static const char *srcs[32];   /* source text of each file, for the excerpt under an error */
static jmp_buf *err_jb; static int nerrors;   /* set while errors are recoverable (checking one function) */
static int ps_depth; static int test_mode;   /* > 0 while a generic is being instantiated: errors there are not recoverable */
static void show_excerpt(int pos) {   /*     12 | let x = a + b;   and a caret under the column */
    int fi = POS_FILE(pos), ln = POS_LINE(pos), col = POS_COL(pos);
    if (!col || fi >= 32 || !srcs[fi]) return;
    const char *p = srcs[fi]; for (int l = 1; l < ln && *p; p++) if (*p == '\n') l++;
    const char *e = p; while (*e && *e != '\n') e++;
    if (e > p && e[-1] == '\r') e--;
    fprintf(stderr, "%5d | %.*s\n      | ", ln, (int)(e - p), p);
    for (int i = 1; i < col && p + i - 1 < e; i++) fputc(p[i - 1] == '\t' ? '\t' : ' ', stderr);
    fputs("^\n", stderr);
}
static void report(const char *kind, int line, const char *fmt, va_list ap) {   /* file:line:col: kind: message */
    int fi = POS_FILE(line), col = POS_COL(line);
    fprintf(stderr, "%s:%d:", fi < nfiles ? files[fi] : g_path, POS_LINE(line));
    if (col) fprintf(stderr, "%d:", col);
    fprintf(stderr, " %s: ", kind);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    show_excerpt(line);
}
static void warn(int line, const char *fmt, ...) { va_list ap; va_start(ap, fmt); report("warning", line, fmt, ap); va_end(ap); }
static void die(int line, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); report("error", line, fmt, ap); va_end(ap);
    if (err_jb && !ps_depth) { nerrors++; longjmp(*err_jb, 1); }
    exit(1);
}
static void *xalloc(size_t n) { void *p = calloc(1, n ? n : 1); if (!p) { fputs("out of memory\n", stderr); exit(2); } return p; }
static char *xstrn(const char *s, size_t n) { char *p = xalloc(n + 1); memcpy(p, s, n); return p; }
#define VPUSH(a, n, c, v) do { if ((n) >= (c)) { (c) = (c) ? (c) * 2 : 8; (a) = realloc((a), (size_t)(c) * sizeof(*(a))); if (!(a)) exit(2); } (a)[(n)++] = (v); } while (0)

/* ============================ lexer ============================ */
typedef enum { TK_EOF, TK_ID, TK_INT, TK_CHAR, TK_STR, TK_OP, TK_FLOAT } TokKind;
typedef struct { TokKind k; char *s; int len; unsigned long long v; int line; } Token;
static Token *toks; static int ntoks, ctoks, tp;

static int hexval(int c) { return isdigit(c) ? c - '0' : tolower(c) - 'a' + 10; }
static int read_escape(const char **p, int line) {
    char c = *(*p)++;
    switch (c) {
    case 'n': return 10; case 't': return 9; case 'r': return 13; case '0': return 0;
    case '\\': return '\\'; case '\'': return '\''; case '"': return '"';
    case 'x': { int v = 0, i; for (i = 0; i < 2 && isxdigit((unsigned char)**p); i++) v = v * 16 + hexval((unsigned char)*(*p)++); return v; }
    }
    die(line, "unknown escape '\\%c'", c); return 0;
}
static int is_float_lit(const char *p) {   /* digits followed by '.digit' or an exponent (not hex/binary, not '..') */
    if (!isdigit((unsigned char)*p) || (p[0] == '0' && (p[1] == 'x' || p[1] == 'X' || p[1] == 'b' || p[1] == 'B'))) return 0;
    while (isdigit((unsigned char)*p) || *p == '_') p++;
    if (p[0] == '.' && isdigit((unsigned char)p[1])) return 1;
    return (p[0] == 'e' || p[0] == 'E') && (isdigit((unsigned char)p[1]) || ((p[1] == '+' || p[1] == '-') && isdigit((unsigned char)p[2])));
}
static void lex(const char *src, int fidx) {
    if (fidx >= 32) { fprintf(stderr, "svalinnc: too many source files (at most 32)\n"); exit(1); }
    srcs[fidx] = src;
    int base = fidx << 26;
    static const char *ops3[] = { "<<=", ">>=", 0 };
    static const char *ops2[] = { "..", "=>", "->", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>",
                                  "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", 0 };
    int line = 1; const char *p = src, *ls = src;   /* ls: start of the current line */
    for (;;) {
        for (;;) {
            if (*p == '\n') { line++; p++; ls = p; }
            else if (isspace((unsigned char)*p)) p++;
            else if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; }
            else if (p[0] == '/' && p[1] == '*') {
                p += 2; while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') { line++; ls = p + 1; } p++; }
                if (!*p) die(base + line, "unterminated comment");
                p += 2;
            } else break;
        }
        if (line > 0xFFFF) die(base + 0xFFFF, "source files can have at most 65535 lines");
        int col = (int)(p - ls) + 1; if (col > 1023) col = 1023;
        Token t; memset(&t, 0, sizeof t); t.line = base + (col << 16) + line;
        if (!*p) { t.k = TK_EOF; VPUSH(toks, ntoks, ctoks, t); return; }
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *b = p; while (isalnum((unsigned char)*p) || *p == '_') p++;
            t.k = TK_ID; t.len = (int)(p - b); t.s = xstrn(b, t.len);
        } else if (is_float_lit(p)) {
            /* floating-point literal: kept as text (the assembler converts it exactly); '_' separators dropped */
            char buf[128]; int n = 0;
            while (isdigit((unsigned char)*p) || *p == '_') { if (*p != '_' && n < 120) buf[n++] = *p; p++; }
            if (*p == '.') { buf[n++] = *p++; while (isdigit((unsigned char)*p) || *p == '_') { if (*p != '_' && n < 120) buf[n++] = *p; p++; } }
            if (*p == 'e' || *p == 'E') { buf[n++] = *p++; if (*p == '+' || *p == '-') buf[n++] = *p++; while (isdigit((unsigned char)*p)) { if (n < 120) buf[n++] = *p; p++; } }
            if (isalpha((unsigned char)*p) || *p == '_') die(t.line, "invalid numeric literal");
            t.k = TK_FLOAT; t.s = xstrn(buf, (size_t)n); t.len = n;
        } else if (isdigit((unsigned char)*p)) {
            unsigned long long v = 0;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; while (isxdigit((unsigned char)*p) || *p == '_') { if (*p != '_') v = v * 16 + hexval((unsigned char)*p); p++; } }
            else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { p += 2; while (*p == '0' || *p == '1' || *p == '_') { if (*p != '_') v = v * 2 + (*p - '0'); p++; } }
            else { while (isdigit((unsigned char)*p) || *p == '_') { if (*p != '_') v = v * 10 + (*p - '0'); p++; } }
            if (isalpha((unsigned char)*p)) die(t.line, "invalid numeric literal");
            t.k = TK_INT; t.v = v;
        } else if (*p == '\'') {
            p++; int c;
            if (*p == '\\') { p++; c = read_escape(&p, t.line); } else c = (unsigned char)*p++;
            if (*p != '\'') die(t.line, "unterminated char literal");
            p++; t.k = TK_CHAR; t.v = (unsigned)c;
        } else if (*p == '"') {
            p++; char *buf = xalloc(strlen(p) + 1); int n = 0;
            while (*p != '"') {
                if (!*p || *p == '\n') die(t.line, "unterminated string literal");
                if (*p == '\\') { p++; buf[n++] = (char)read_escape(&p, t.line); } else buf[n++] = *p++;
            }
            p++; t.k = TK_STR; t.s = buf; t.len = n;
        } else {
            int i, found = 0;
            for (i = 0; ops3[i]; i++) if (!strncmp(p, ops3[i], 3)) { t.s = xstrn(p, 3); p += 3; found = 1; break; }
            if (!found) for (i = 0; ops2[i]; i++) if (!strncmp(p, ops2[i], 2)) { t.s = xstrn(p, 2); p += 2; found = 1; break; }
            if (!found) {
                if (!strchr("+-*/%&|^~!<>=(){}[];:,.@?", *p)) die(t.line, "unexpected character '%c'", *p);
                t.s = xstrn(p, 1); p++;
            }
            t.k = TK_OP;
        }
        VPUSH(toks, ntoks, ctoks, t);
    }
}

/* ============================ types ============================ */
typedef enum { TY_VOID, TY_BOOL, TY_INT, TY_STR, TY_PTR, TY_STRUCT, TY_ARRAY, TY_ENUM, TY_ILIT, TY_NULL, TY_RESULT, TY_RESPEND, TY_PARAM, TY_IFACE, TY_FLOAT, TY_FLIT, TY_TASK, TY_DATA } TyKind;   /* TY_DATA: enum whose variants carry values */
typedef struct Type Type; typedef struct StructDef StructDef; typedef struct EnumDef EnumDef;
typedef struct Expr Expr; typedef struct Generic Generic; typedef struct Iface Iface; typedef struct Func Func;
typedef struct { char *name; Type *ty; int off; int is_mut, vis; Expr *init; StructDef *owner; } Field;   /* vis: 0 public 1 private 2 protected 3 internal */
struct Type { TyKind k; int size, align, sign, is_char; Type *elem; long long n; StructDef *sd; EnumDef *ed; Type *err; Iface *ifc; int opt; };   /* opt: Option<T> (a Result<T, void> spelled Some/None) */
struct StructDef { char *name; Field *f; int nf, cf; int done; int is_class; Type *ty; Generic *gen; Type **targs; StructDef *base; Iface **ifs; int nifs, cifs, line; Func *dtor; };
struct Iface { char *name; Type *ty; Func **m; int nm, cm; };   /* interface: methods are abstract Funcs; values are {object*, vtable*} */
static Iface **ifaces; static int nifaces, cifaces;
static Iface *find_iface(const char *n) { for (int i = 0; i < nifaces; i++) if (!strcmp(ifaces[i]->name, n)) return ifaces[i]; return 0; }
static int is_sub(StructDef *a, StructDef *b) { for (; a; a = a->base) if (a == b) return 1; return 0; }   /* a is b or derives from b */
#define MAXVF 8   /* values per enum variant */
typedef struct { int nf; Type *ft[MAXVF]; int off[MAXVF]; } VarInfo;   /* a variant's values: types and offsets (after the 8-byte tag) */
struct EnumDef { char *name; char **vn; int nv, cv; Type *ty; int data, done; VarInfo *vi; int nvi, cvi; };   /* data: some variant carries values */
static int has_drop(Type *t) {   /* values of this type own something and must be destroyed */
    if (t->k == TY_TASK || t->k == TY_STR) return 1;   /* dropping a task waits for it; strings are reference counted */
    if (t->k == TY_ARRAY) return has_drop(t->elem);
    if (t->k == TY_RESULT) return has_drop(t->elem) || has_drop(t->err);
    if (t->k == TY_DATA) { for (int v = 0; v < t->ed->nvi; v++) for (int j = 0; j < t->ed->vi[v].nf; j++) if (has_drop(t->ed->vi[v].ft[j])) return 1; }
    if (t->k == TY_STRUCT) {
        for (StructDef *c = t->sd; c; c = c->base) if (c->dtor) return 1;
        for (int i = 0; i < t->sd->nf; i++) if (has_drop(t->sd->f[i].ty)) return 1;
    }
    return 0;
}
static int move_only(Type *t) {   /* moved, never copied: destructors and tasks (strings are copied by counting references) */
    if (t->k == TY_TASK) return 1;
    if (t->k == TY_ARRAY) return move_only(t->elem);
    if (t->k == TY_RESULT) return move_only(t->elem) || move_only(t->err);
    if (t->k == TY_DATA) { for (int v = 0; v < t->ed->nvi; v++) for (int j = 0; j < t->ed->vi[v].nf; j++) if (move_only(t->ed->vi[v].ft[j])) return 1; }
    if (t->k == TY_STRUCT) {
        for (StructDef *c = t->sd; c; c = c->base) if (c->dtor) return 1;
        for (int i = 0; i < t->sd->nf; i++) if (move_only(t->sd->f[i].ty)) return 1;
    }
    return 0;
}
static int implements(StructDef *sd, Iface *ifc) { for (; sd; sd = sd->base) for (int i = 0; i < sd->nifs; i++) if (sd->ifs[i] == ifc) return 1; return 0; }

static Type ty_void = { TY_VOID, 0, 1 }, ty_bool = { TY_BOOL, 1, 1 }, ty_str = { TY_STR, 8, 8 }, ty_ilit = { TY_ILIT, 8, 8, 1 };
static Type ty_null = { TY_NULL, 8, 8, 0, 0 };
static Type ty_i8 = { TY_INT, 1, 1, 1 }, ty_i16 = { TY_INT, 2, 2, 1 }, ty_i32 = { TY_INT, 4, 4, 1 }, ty_i64 = { TY_INT, 8, 8, 1 };
static Type ty_u8 = { TY_INT, 1, 1, 0 }, ty_u16 = { TY_INT, 2, 2, 0 }, ty_u32 = { TY_INT, 4, 4, 0 }, ty_u64 = { TY_INT, 8, 8, 0 };
static Type ty_char = { TY_INT, 1, 1, 0, 1 };
static Type ty_f32 = { TY_FLOAT, 4, 4 }, ty_f64 = { TY_FLOAT, 8, 8 }, ty_flit = { TY_FLIT, 8, 8 };

static StructDef **structs; static int nstructs, cstructs;
static EnumDef **enums; static int nenums, cenums;

static Type *mk_ptr(Type *e) { Type *t = xalloc(sizeof *t); t->k = TY_PTR; t->size = t->align = 8; t->elem = e; return t; }
static Type *mk_arr(Type *e, long long n) { Type *t = xalloc(sizeof *t); t->k = TY_ARRAY; t->elem = e; t->n = n; t->size = (int)(e->size * n); t->align = e->align; return t; }
static int is_int(Type *t) { return t->k == TY_INT; }
static int is_float(Type *t) { return t->k == TY_FLOAT; }
static Type *mk_result(Type *t, Type *e) {
    Type *r = xalloc(sizeof *r); int pl = t->size > e->size ? t->size : e->size;
    r->k = TY_RESULT; r->elem = t; r->err = e; r->align = 8; r->size = 8 + (pl + 7) / 8 * 8; return r;   /* tag (0 Ok, 1 Err) + payload */
}
static Type *mk_option(Type *t) { Type *r = mk_result(t, &ty_void); r->opt = 1; return r; }   /* tag 0 Some, 1 None */
static int is_agg(Type *t) { return t->k == TY_STRUCT || t->k == TY_ARRAY || t->k == TY_RESULT || t->k == TY_IFACE || t->k == TY_TASK || t->k == TY_DATA; }
static Type *mk_task(Type *e) { Type *t = xalloc(sizeof *t); t->k = TY_TASK; t->elem = e; t->size = t->align = 8; return t; }   /* handle of a running async call */
static const char *IFACE_MSG = "interface type '%s' can only be used as a function parameter (it borrows the object)";
static int has_iface(Type *t) {
    if (t->k == TY_IFACE) return 1;
    if (t->k == TY_PTR || t->k == TY_ARRAY) return has_iface(t->elem);
    if (t->k == TY_RESULT) return has_iface(t->elem) || has_iface(t->err);
    return 0;
}
static int has_ptr(Type *t) {
    if (t->k == TY_DATA) { for (int v = 0; v < t->ed->nvi; v++) for (int j = 0; j < t->ed->vi[v].nf; j++) if (has_ptr(t->ed->vi[v].ft[j])) return 1; return 0; }
    return t->k == TY_PTR || (t->k == TY_ARRAY && has_ptr(t->elem)) || (t->k == TY_RESULT && (has_ptr(t->elem) || has_ptr(t->err)));
}

static int same(Type *a, Type *b) {
    if (a == b) return 1;
    if (a->k != b->k) return 0;
    switch (a->k) {
    case TY_INT: return a->size == b->size && a->sign == b->sign;
    case TY_FLOAT: return a->size == b->size;
    case TY_PTR: return same(a->elem, b->elem);
    case TY_ARRAY: return a->n == b->n && same(a->elem, b->elem);
    case TY_STRUCT: return a->sd == b->sd;
    case TY_ENUM: case TY_DATA: return a->ed == b->ed;
    case TY_RESULT: return a->opt == b->opt && same(a->elem, b->elem) && same(a->err, b->err);
    case TY_IFACE: return a->ifc == b->ifc;
    case TY_TASK: return same(a->elem, b->elem);
    default: return 1;
    }
}
static char *tname(Type *t) {
    char *b = xalloc(160);
    switch (t->k) {
    case TY_VOID: strcpy(b, "void"); break; case TY_BOOL: strcpy(b, "bool"); break;
    case TY_NULL: strcpy(b, "null"); break;
    case TY_STR: strcpy(b, "String"); break; case TY_ILIT: strcpy(b, "integer literal"); break;
    case TY_FLOAT: sprintf(b, "f%d", t->size * 8); break; case TY_FLIT: strcpy(b, "float literal"); break;
    case TY_INT: if (t->is_char) strcpy(b, "char"); else sprintf(b, "%c%d", t->sign ? 'i' : 'u', t->size * 8); break;
    case TY_PTR: sprintf(b, "*%.120s", tname(t->elem)); break;
    case TY_ARRAY: sprintf(b, "[%lld]%.100s", t->n, tname(t->elem)); break;
    case TY_STRUCT: sprintf(b, "%.150s", t->sd->name); break; case TY_ENUM: case TY_DATA: sprintf(b, "%.150s", t->ed->name); break;
    case TY_RESULT:
        if (t->opt) { sprintf(b, "Option<%.140s>", tname(t->elem)); break; }
        { char *a = tname(t->elem), *c = tname(t->err); sprintf(b, "Result<%.70s, %.70s>", a, c); break; }
    case TY_RESPEND:
        if (t->opt && t->n) { strcpy(b, "None"); break; }
        sprintf(b, "%s(%.140s)", t->opt ? "Some" : t->n ? "Err" : "Ok", tname(t->elem)); break;
    case TY_PARAM: strcpy(b, "type parameter"); break;
    case TY_IFACE: sprintf(b, "%.150s", t->ifc->name); break;
    case TY_TASK: sprintf(b, "Task<%.140s>", tname(t->elem)); break;
    }
    return b;
}
static StructDef *find_struct(const char *n) { for (int i = 0; i < nstructs; i++) if (!strcmp(structs[i]->name, n)) return structs[i]; return 0; }
static EnumDef *find_enum(const char *n) { for (int i = 0; i < nenums; i++) if (!strcmp(enums[i]->name, n)) return enums[i]; return 0; }
static void need_complete(Type *t, int line) {
    if (t->k == TY_STRUCT && !t->sd->done) die(line, "struct '%s' is incomplete here", t->sd->name);
    if (t->k == TY_DATA && !t->ed->done) die(line, "enum '%s' is incomplete here", t->ed->name);
    if (t->k == TY_ARRAY) need_complete(t->elem, line);
    if (t->k == TY_RESULT) { need_complete(t->elem, line); need_complete(t->err, line); }
    if (t->k == TY_TASK) need_complete(t->elem, line);
}

/* ============================ AST ============================ */
typedef enum { E_INT, E_BOOL, E_STR, E_VAR, E_CALL, E_BIN, E_UN, E_CAST, E_INDEX, E_FIELD, E_REF, E_ENUMVAL, E_SIZEOF, E_MCALL, E_OK, E_ERR, E_TRY, E_TOIFACE, E_FLOAT, E_AWAIT, E_SLIT, E_FINIT, E_VARIANT } EKind;   /* E_VARIANT: Enum.Variant(values) of an enum with data, ival = variant */
typedef enum { O_ADD, O_SUB, O_MUL, O_DIV, O_MOD, O_AND, O_OR, O_XOR, O_SHL, O_SHR,
               O_EQ, O_NE, O_LT, O_LE, O_GT, O_GE, O_LAND, O_LOR, O_NEG, O_NOT, O_BNOT, O_DEREF, O_ADDR } Op;
typedef struct Stmt Stmt; typedef struct Local Local;
struct Local { char *name; Type *ty; int off, tmp, is_ref, is_mut; int is_global; Expr *init; int line; int dflag, moved, loop;
    int reg, uses, addr_taken, seen, has_range; long long lo, hi;
    int is_cst, nasg, ifirst, ilast; long long cval;
    int is_const; };   /* 'const' global: uses are replaced by its literal */   /* optimizer: register (1-5 GP, 8-15 xmm), weighted uses, known value range */   /* dflag: drop-flag slot (alive) */
struct Expr {
    EKind k; int line, op, neg; Expr *a, *b; Expr **args; int nargs, cargs;
    char *name; long long ival; char *sval; int slen; Type *ty; Type *cast_ty;
    Local *lv; Field *fld; Func *fn; int builtin, tmp, cls, recv;
    int move, consumed, dflag; Expr *tnext;   /* ownership: moved-from variable / temporary taken over / temporary drop flag */
    Expr *inl; int inl_done;   /* optimizer: inlined body of this call */
    int copy;   /* the value of this place is copied (reference counts incremented) */
};
typedef enum { S_LET, S_ASSIGN, S_EXPR, S_IF, S_WHILE, S_FOR, S_RET, S_BRK, S_CONT, S_BLOCK, S_MATCH } SKind;
struct Stmt {
    SKind k; int line; char *name; int is_mut, unsafe_blk, is_range;
    Type *dty; Expr *e, *e2; Stmt *body, *els; Stmt **list; int n, cap; Expr **pats; Local **alv;
    Local *lv; int slot1; Expr *temps; Local *bnd;   /* bnd: for-loop bound (optimizer) */   /* temporaries with destructors created by this statement */
    int init_store;   /* constructor: first assignment of a field (it is still zero, nothing to drop) */
    Local ***vlv;   /* match on an enum with data: the bindings of each arm (MAXVF per arm) */
};
#define MAXPARAMS 16
struct Func {
    char *name; int line, nparams, is_unsafe, is_extern, frame, retslot;
    char **pname; Type **pty; int *pref; Local **plocal;
    StructDef *cls; int is_ctor, is_static, is_method, vis;
    Iface *ifc; int vt_idx;   /* interface method (abstract): index in the vtable */
    int is_dtor, is_async;
    int nsaved; char saved[8];   /* optimizer: callee-saved registers used */
    int inl_state; Expr *inl_body;   /* optimizer: 1 = small enough to inline, 2 = not */
    int borrow;   /* runtime prelude function: String parameters are borrowed, not owned */
    char *req[8]; int nreq;   /* `requires A.B, C`: capabilities a caller must have */
    int is_test, is_deprecated;   /* @test, @deprecated */
    Type *ret; Stmt *body;
};

static Func **funcs; static int nfuncs, cfuncs;
static Local **globals; static int nglobals, cglobals;
static char *mod_name = "main";
static char **allow_caps, **deny_caps; static int nallow, callow, ndeny, cdeny;
static int protect_process;

/* ============================ parser ============================ */
static Token *cur(void) { return &toks[tp]; }
static int isop(const char *s) { return toks[tp].k == TK_OP && !strcmp(toks[tp].s, s); }
static int iskw(const char *s) { return toks[tp].k == TK_ID && !strcmp(toks[tp].s, s); }
static int acceptop(const char *s) { if (isop(s)) { tp++; return 1; } return 0; }
static int acceptkw(const char *s) { if (iskw(s)) { tp++; return 1; } return 0; }
static void expectop(const char *s) { if (!acceptop(s)) die(cur()->line, "expected '%s'", s); }
static char *expect_id(void) {
    if (cur()->k != TK_ID) die(cur()->line, "expected identifier");
    return toks[tp++].s;
}
static char *parse_dotted(void) {
    char buf[256]; strcpy(buf, expect_id());
    while (isop(".")) { tp++; strcat(buf, "."); strcat(buf, expect_id()); }
    return xstrn(buf, strlen(buf));
}

/* ---- generics: templates are token ranges, instantiated (monomorphised) by re-parsing with type parameters bound ---- */
struct Generic { char *name; int kind, np, start, is_unsafe, is_async, line; char *pn[8]; char *pc[8]; };   /* pc: constraint (interface name) or 0 */   /* kind: 0 class, 1 struct, 2 fn */
static Generic **gens; static int ngens, cgens;
static Generic *find_generic(const char *n) { for (int i = 0; i < ngens; i++) if (!strcmp(gens[i]->name, n)) return gens[i]; return 0; }
static char *te_name[8]; static Type *te_ty[8]; static int nte, pend_gt;
typedef struct { int tp, nte, pend; char *nm[8]; Type *ty[8]; } PState;
static void ps_save(PState *p) { ps_depth++; p->tp = tp; p->nte = nte; p->pend = pend_gt; memcpy(p->nm, te_name, sizeof te_name); memcpy(p->ty, te_ty, sizeof te_ty); }
static void ps_restore(PState *p) { ps_depth--; tp = p->tp; nte = p->nte; pend_gt = p->pend; memcpy(te_name, p->nm, sizeof te_name); memcpy(te_ty, p->ty, sizeof te_ty); }
static void check_constraints(Generic *g, Type **args, int line) {
    for (int i = 0; i < g->np; i++) {
        if (!g->pc[i]) continue;
        Iface *ifc = find_iface(g->pc[i]);
        if (!ifc) die(g->line, "unknown interface '%s' in constraint", g->pc[i]);
        if (!(args[i]->k == TY_STRUCT && args[i]->sd->is_class && implements(args[i]->sd, ifc)))
            die(line, "type '%s' does not implement '%s' (required by %s)", tname(args[i]), ifc->name, g->name);
    }
}
static void ps_bind(Generic *g, Type **args) { nte = g->np; pend_gt = 0; for (int i = 0; i < g->np; i++) { te_name[i] = g->pn[i]; te_ty[i] = args[i]; } }
static void expect_gt(void) {   /* closes '<' ... '>' ; splits '>>' into two closers without touching the token */
    if (pend_gt) { pend_gt = 0; return; }
    if (isop(">>")) { tp++; pend_gt = 1; return; }
    expectop(">");
}
static Type *parse_type(void);
static void parse_class_body(StructDef *sd, int line);
static void parse_struct_body(StructDef *sd, int line);
static void parse_fn_ex(StructDef *cls, int is_unsafe, int is_extern, int is_static, int vis, int is_ctor, char *force, Iface *ifc);
static int parse_type_args(Generic *g, Type **out, int line) {
    expectop("<"); int n = 0;
    for (;;) {
        if (n >= 8) die(line, "too many type arguments");
        out[n++] = parse_type();
        if (!acceptop(",")) break;
    }
    expect_gt();
    if (n != g->np) die(line, "'%s' expects %d type argument(s), got %d", g->name, g->np, n);
    return n;
}
static char *inst_name(Generic *g, Type **args) {
    char nm[400]; int o = snprintf(nm, sizeof nm, "%s<", g->name);
    for (int i = 0; i < g->np; i++) o += snprintf(nm + o, sizeof nm - (size_t)o, "%s%s", i ? ", " : "", tname(args[i]));
    snprintf(nm + o, sizeof nm - (size_t)o, ">"); return xstrn(nm, strlen(nm));
}
static int has_param(Type *t) {
    if (t->k == TY_PARAM) return 1;
    if (t->k == TY_PTR || t->k == TY_ARRAY) return has_param(t->elem);
    if (t->k == TY_RESULT) return has_param(t->elem) || has_param(t->err);
    if (t->k == TY_STRUCT && t->sd->gen) for (int i = 0; i < t->sd->gen->np; i++) if (has_param(t->sd->targs[i])) return 1;
    return 0;
}
static Type *instantiate_type(Generic *g, Type **args, int line) {
    char *nm = inst_name(g, args); StructDef *sd = find_struct(nm);
    if (sd) return sd->ty;
    sd = xalloc(sizeof *sd); sd->name = nm; sd->ty = xalloc(sizeof(Type)); sd->ty->k = TY_STRUCT; sd->ty->sd = sd; sd->ty->size = 1; sd->ty->align = 1;
    sd->gen = g; sd->targs = xalloc(sizeof(Type *) * 8); for (int i = 0; i < g->np; i++) sd->targs[i] = args[i];
    for (int i = 0; i < g->np; i++) if (has_param(args[i])) { sd->is_class = g->kind == 0; return sd->ty; }   /* shell used only for argument inference */
    check_constraints(g, args, line);
    VPUSH(structs, nstructs, cstructs, sd);
    PState ps; ps_save(&ps); ps_bind(g, args); tp = g->start;
    if (g->kind == 0) parse_class_body(sd, line); else parse_struct_body(sd, line);
    ps_restore(&ps); return sd->ty;
}
static Func *instantiate_fn(Generic *g, Type **args, int line) {
    char *nm = inst_name(g, args);
    check_constraints(g, args, line);
    for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, nm)) return funcs[i];
    PState ps; ps_save(&ps); ps_bind(g, args); tp = g->start;
    int nf0 = nfuncs;
    parse_fn_ex(0, g->is_unsafe, 0, 0, 0, 0, nm, 0);
    if (nfuncs > nf0) funcs[nfuncs - 1]->is_async = g->is_async;
    ps_restore(&ps); return funcs[nfuncs - 1];
}

static int const_int(const char *n, long long *v) {   /* value of an integer constant declared so far */
    for (int i = 0; i < nglobals; i++) if (globals[i]->is_const && !strcmp(globals[i]->name, n) && globals[i]->init->k == E_INT) { *v = globals[i]->init->ival; return 1; }
    return 0;
}
static int const_eval(Expr *e, long long *v) {   /* integer constant expression of literals and constants */
    long long a, b;
    switch (e->k) {
    case E_INT: if (e->cls) return 0; *v = e->ival; return 1;
    case E_VAR: return const_int(e->name, v);
    case E_UN: if (e->op != O_NEG || !const_eval(e->a, &a)) return 0; *v = (long long)(0ULL - (unsigned long long)a); return 1;
    case E_BIN:
        if (!const_eval(e->a, &a) || !const_eval(e->b, &b)) return 0;
        switch (e->op) {
        case O_ADD: *v = (long long)((unsigned long long)a + (unsigned long long)b); return 1;
        case O_SUB: *v = (long long)((unsigned long long)a - (unsigned long long)b); return 1;
        case O_MUL: *v = (long long)((unsigned long long)a * (unsigned long long)b); return 1;
        case O_DIV: if (b == 0 || (b == -1 && a == (long long)(1ULL << 63))) return 0; *v = a / b; return 1;
        case O_MOD: if (b == 0 || b == -1) return 0; *v = a % b; return 1;
        case O_AND: *v = a & b; return 1; case O_OR: *v = a | b; return 1; case O_XOR: *v = a ^ b; return 1;
        case O_SHL: if (b < 0 || b > 63) return 0; *v = (long long)((unsigned long long)a << b); return 1;
        case O_SHR: if (b < 0 || b > 63) return 0; *v = a >> b; return 1;
        default: return 0;
        }
    default: return 0;
    }
}
static Type *parse_type(void) {
    int line = cur()->line;
    if (acceptop("*")) return mk_ptr(parse_type());
    if (acceptop("[")) {
        long long n = 0;
        if (cur()->k == TK_INT) n = (long long)toks[tp++].v;
        else if (cur()->k == TK_ID && const_int(cur()->s, &n)) tp++;
        else die(line, "array length must be an integer literal or a constant defined above");
        expectop("]");
        Type *e = parse_type(); need_complete(e, line);
        if (n <= 0) die(line, "array length must be positive");
        return mk_arr(e, n);
    }
    char *n = expect_id();
    if (!strcmp(n, "Task")) {
        expectop("<"); Type *et = parse_type(); expect_gt(); need_complete(et, line); return mk_task(et);
    }
    if (!strcmp(n, "Result")) {
        expectop("<"); Type *rt = parse_type(); expectop(","); Type *re = parse_type();
        expect_gt();
        need_complete(rt, line); need_complete(re, line); return mk_result(rt, re);
    }
    if (!strcmp(n, "Option")) {
        expectop("<"); Type *ot = parse_type(); expect_gt();
        if (ot->k == TY_VOID) die(line, "Option needs a value type, not 'void'");
        need_complete(ot, line); return mk_option(ot);
    }
    if (!strcmp(n, "bool")) return &ty_bool;
    if (!strcmp(n, "char")) return &ty_char;
    if (!strcmp(n, "String")) return &ty_str;
    if (!strcmp(n, "void")) return &ty_void;
    if (!strcmp(n, "i8")) return &ty_i8;   if (!strcmp(n, "i16")) return &ty_i16;
    if (!strcmp(n, "i32")) return &ty_i32; if (!strcmp(n, "i64") || !strcmp(n, "isize")) return &ty_i64;
    if (!strcmp(n, "u8")) return &ty_u8;   if (!strcmp(n, "u16")) return &ty_u16;
    if (!strcmp(n, "u32")) return &ty_u32; if (!strcmp(n, "u64") || !strcmp(n, "usize")) return &ty_u64;
    if (!strcmp(n, "f32")) return &ty_f32;
    if (!strcmp(n, "f64")) return &ty_f64;
    for (int i = 0; i < nte; i++) if (!strcmp(te_name[i], n)) return te_ty[i];
    Generic *gn = find_generic(n);
    if (gn) {
        if (gn->kind == 2) die(line, "'%s' is a generic function, not a type", n);
        if (!isop("<")) die(line, "generic type '%s' needs type arguments, e.g. %s<i32>", n, n);
        Type *ta[8]; parse_type_args(gn, ta, line); return instantiate_type(gn, ta, line);
    }
    StructDef *sd = find_struct(n); if (sd) return sd->ty;
    EnumDef *ed = find_enum(n); if (ed) return ed->ty;
    Iface *ifc = find_iface(n); if (ifc) return ifc->ty;
    die(line, "unknown type '%s'", n); return 0;
}

static Expr *mk(EKind k, int line) { Expr *e = xalloc(sizeof *e); e->k = k; e->line = line; return e; }
static Expr *parse_expr(void);
static Expr *parse_unary(void);
static int no_slit;   /* parsing an if/while/match/for header: 'Name {' starts the body, not a struct literal */
static Expr *parse_expr_free(void) { int sv = no_slit; no_slit = 0; Expr *e = parse_expr(); no_slit = sv; return e; }
static Expr *parse_expr_head(void) { int sv = no_slit; no_slit = 1; Expr *e = parse_expr(); no_slit = sv; return e; }
static StructDef *find_struct(const char *n);
static Expr *parse_struct_lit(char *nm, int line) {   /* Name { field: value, field, ... } */
    Expr *e = mk(E_SLIT, line); e->name = nm; expectop("{");
    while (!isop("}")) {
        int fl = cur()->line; Expr *fi = mk(E_FINIT, fl); fi->name = expect_id();
        if (acceptop(":")) fi->a = parse_expr_free();
        else { fi->a = mk(E_VAR, fl); fi->a->name = fi->name; }   /* shorthand: { x } means { x: x } */
        VPUSH(e->args, e->nargs, e->cargs, fi);
        if (!acceptop(",")) break;
    }
    expectop("}"); return e;
}

static Expr *parse_primary(void) {
    Token *t = cur(); int line = t->line; Expr *e;
    if (t->k == TK_INT) { tp++; e = mk(E_INT, line); e->ival = (long long)t->v; return e; }
    if (t->k == TK_FLOAT) { tp++; e = mk(E_FLOAT, line); e->sval = t->s; return e; }
    if (t->k == TK_CHAR) { tp++; e = mk(E_INT, line); e->ival = (long long)t->v; e->ty = &ty_char; e->cls = 1; return e; }
    if (t->k == TK_STR) { tp++; e = mk(E_STR, line); e->sval = t->s; e->slen = t->len; return e; }
    if (t->k == TK_ID) {
        tp++;
        if (!strcmp(t->s, "true") || !strcmp(t->s, "false")) { e = mk(E_BOOL, line); e->ival = t->s[0] == 't'; return e; }
        if (!strcmp(t->s, "null")) { e = mk(E_INT, line); e->ty = &ty_null; e->cls = 2; return e; }
        if (!strcmp(t->s, "sizeof") && isop("(")) { tp++; e = mk(E_SIZEOF, line); e->cast_ty = parse_type(); expectop(")"); return e; }
        char *nm = t->s; Generic *gn = find_generic(nm);
        if (gn && isop("<")) {   /* List<i32>(..), Vec<i32>.make(), max<i32>(..) */
            Type *ta[8]; parse_type_args(gn, ta, line);
            if (gn->kind == 2) { if (!isop("(")) die(line, "generic function '%s' must be called", gn->name); nm = instantiate_fn(gn, ta, line)->name; }
            else nm = instantiate_type(gn, ta, line)->sd->name;
        }
        if (isop("(")) {
            tp++; e = mk(E_CALL, line); e->name = nm;
            if (!isop(")")) do { VPUSH(e->args, e->nargs, e->cargs, parse_expr_free()); } while (acceptop(","));
            expectop(")"); return e;
        }
        if (isop("{") && !no_slit) { StructDef *sd = find_struct(nm); if (sd && !sd->is_class) return parse_struct_lit(nm, line); }
        e = mk(E_VAR, line); e->name = nm; return e;
    }
    if (acceptop("(")) { e = parse_expr_free(); expectop(")"); return e; }
    die(line, "expected expression"); return 0;
}
static Expr *parse_postfix(void) {
    Expr *e = parse_primary();
    for (;;) {
        int line = cur()->line;
        if (acceptop("[")) { Expr *i = mk(E_INDEX, line); i->a = e; i->b = parse_expr_free(); expectop("]"); e = i; }
        else if (isop(".")) {
            tp++; char *fname = expect_id();
            if (isop("(")) {
                tp++; Expr *m = mk(E_MCALL, line); m->a = e; m->name = fname;
                if (!isop(")")) do { VPUSH(m->args, m->nargs, m->cargs, parse_expr_free()); } while (acceptop(","));
                expectop(")"); e = m;
            } else { Expr *f = mk(E_FIELD, line); f->a = e; f->name = fname; e = f; }
        }
        else if (isop("?")) { tp++; Expr *t = mk(E_TRY, line); t->a = e; e = t; }
        else break;
    }
    return e;
}
static Expr *parse_unary(void) {
    int line = cur()->line; Expr *e;
    if (acceptop("-")) {
        Expr *a = parse_unary();
        if (a->k == E_INT && !a->ty) { a->ival = -a->ival; a->neg = 1; return a; }
        if (a->k == E_FLOAT) {   /* fold the sign into the literal text */
            if (a->sval[0] == '-') a->sval = a->sval + 1;
            else { char *t2 = xalloc(strlen(a->sval) + 2); t2[0] = '-'; strcpy(t2 + 1, a->sval); a->sval = t2; }
            return a;
        }
        e = mk(E_UN, line); e->op = O_NEG; e->a = a; return e;
    }
    if (acceptop("!")) { e = mk(E_UN, line); e->op = O_NOT; e->a = parse_unary(); return e; }
    if (acceptop("~")) { e = mk(E_UN, line); e->op = O_BNOT; e->a = parse_unary(); return e; }
    if (acceptop("*")) { e = mk(E_UN, line); e->op = O_DEREF; e->a = parse_unary(); return e; }
    if (acceptop("&")) { e = mk(E_UN, line); e->op = O_ADDR; e->a = parse_unary(); return e; }
    if (acceptkw("ref")) { e = mk(E_REF, line); e->a = parse_unary(); return e; }
    if (acceptkw("await")) { e = mk(E_AWAIT, line); e->a = parse_unary(); return e; }
    return parse_postfix();
}
static Expr *parse_cast(void) {
    Expr *e = parse_unary();
    while (iskw("as")) { int line = cur()->line; tp++; Expr *c = mk(E_CAST, line); c->a = e; c->cast_ty = parse_type(); e = c; }
    return e;
}
static struct { const char *s; int prec, op; } bin_tab[] = {
    { "||", 1, O_LOR }, { "&&", 2, O_LAND }, { "|", 3, O_OR }, { "^", 4, O_XOR }, { "&", 5, O_AND },
    { "==", 6, O_EQ }, { "!=", 6, O_NE }, { "<", 7, O_LT }, { "<=", 7, O_LE }, { ">", 7, O_GT }, { ">=", 7, O_GE },
    { "<<", 8, O_SHL }, { ">>", 8, O_SHR }, { "+", 9, O_ADD }, { "-", 9, O_SUB },
    { "*", 10, O_MUL }, { "/", 10, O_DIV }, { "%", 10, O_MOD }, { 0, 0, 0 } };
static Expr *parse_binary(int minprec) {
    Expr *l = parse_cast();
    for (;;) {
        int i, found = -1;
        if (cur()->k == TK_OP) for (i = 0; bin_tab[i].s; i++) if (!strcmp(cur()->s, bin_tab[i].s) && bin_tab[i].prec >= minprec) { found = i; break; }
        if (found < 0) return l;
        int line = cur()->line; tp++;
        Expr *r = parse_binary(bin_tab[found].prec + 1);
        Expr *b = mk(E_BIN, line); b->op = bin_tab[found].op; b->a = l; b->b = r; l = b;
    }
}
static Expr *parse_expr(void) { return parse_binary(1); }

static Stmt *mks(SKind k, int line) { Stmt *s = xalloc(sizeof *s); s->k = k; s->line = line; return s; }
static Stmt *parse_stmt(void);
static Stmt *parse_block(void) {
    Stmt *b = mks(S_BLOCK, cur()->line); expectop("{");
    while (!isop("}")) { if (cur()->k == TK_EOF) die(cur()->line, "unexpected end of file in block"); VPUSH(b->list, b->n, b->cap, parse_stmt()); }
    tp++; return b;
}
static Stmt *parse_stmt(void) {
    int line = cur()->line; Stmt *s;
    if (isop("{")) return parse_block();
    if (iskw("unsafe") && toks[tp + 1].k == TK_OP && !strcmp(toks[tp + 1].s, "{")) { tp++; s = parse_block(); s->unsafe_blk = 1; return s; }
    if (iskw("let") || iskw("var") || iskw("const")) {
        s = mks(S_LET, line); s->is_mut = iskw("var"); int is_c = iskw("const"); tp++; s->name = expect_id();
        if (acceptop(":")) s->dty = parse_type();
        if (acceptop("=")) s->e = parse_expr();
        if (!s->dty && !s->e) die(line, "declaration of '%s' needs a type or an initializer", s->name);
        if (is_c && !s->e) die(line, "constant '%s' needs a value", s->name);
        expectop(";"); return s;
    }
    if (acceptkw("if")) {
        s = mks(S_IF, line); s->e = parse_expr_head(); s->body = parse_block();
        if (acceptkw("else")) { if (iskw("if")) s->els = parse_stmt(); else s->els = parse_block(); }
        return s;
    }
    if (acceptkw("while")) { s = mks(S_WHILE, line); s->e = parse_expr_head(); s->body = parse_block(); return s; }
    if (acceptkw("for")) {
        s = mks(S_FOR, line); s->name = expect_id();
        if (!acceptkw("in")) die(line, "expected 'in'");
        s->e = parse_expr_head();
        if (acceptop("..")) { s->is_range = 1; s->e2 = parse_expr_head(); }
        s->body = parse_block(); return s;
    }
    if (acceptkw("return")) { s = mks(S_RET, line); if (!isop(";")) s->e = parse_expr(); expectop(";"); return s; }
    if (acceptkw("break")) { s = mks(S_BRK, line); expectop(";"); return s; }
    if (acceptkw("continue")) { s = mks(S_CONT, line); expectop(";"); return s; }
    if (acceptkw("match")) {
        s = mks(S_MATCH, line); s->e = parse_expr_head(); expectop("{");
        while (!isop("}")) {
            Expr *pat = 0;
            if (iskw("_")) tp++; else pat = parse_expr();
            expectop("=>");
            Stmt *body;
            if (isop("{")) body = parse_block();
            else { body = mks(S_EXPR, cur()->line); body->e = parse_expr(); }
            acceptop(",");
            if (s->n >= s->cap) {
                s->cap = s->cap ? s->cap * 2 : 8;
                s->pats = realloc(s->pats, (size_t)s->cap * sizeof *s->pats);
                s->list = realloc(s->list, (size_t)s->cap * sizeof *s->list);
            }
            s->pats[s->n] = pat; s->list[s->n++] = body;
        }
        tp++; return s;
    }
    Expr *lhs = parse_expr();
    static struct { const char *s; int op; } asg[] = { { "+=", O_ADD }, { "-=", O_SUB }, { "*=", O_MUL }, { "/=", O_DIV }, { "%=", O_MOD },
        { "&=", O_AND }, { "|=", O_OR }, { "^=", O_XOR }, { "<<=", O_SHL }, { ">>=", O_SHR }, { 0, 0 } };
    if (acceptop("=")) { s = mks(S_ASSIGN, line); s->e = lhs; s->e2 = parse_expr(); expectop(";"); return s; }
    for (int i = 0; asg[i].s; i++) if (isop(asg[i].s)) {
        tp++; s = mks(S_ASSIGN, line); s->e = lhs;
        Expr *b = mk(E_BIN, line); b->op = asg[i].op; b->a = lhs; b->b = parse_expr(); s->e2 = b;
        expectop(";"); return s;
    }
    s = mks(S_EXPR, line); s->e = lhs; expectop(";"); return s;
}

static void skip_block(void) {   /* at '{': skip to after the matching '}' */
    int depth = 0;
    for (;;) {
        if (cur()->k == TK_EOF) die(cur()->line, "unexpected end of file");
        if (isop("{")) depth++;
        else if (isop("}")) { depth--; if (depth == 0) { tp++; return; } }
        tp++;
    }
}
static void skip_type_params(void) { while (!isop(">")) { if (cur()->k == TK_EOF) die(cur()->line, "malformed type parameter list"); tp++; } tp++; }
static void parse_struct_body(StructDef *sd, int line);
static void parse_struct(void) {
    int line = cur()->line; char *n = expect_id();
    if (isop("<")) { skip_type_params(); skip_block(); return; }   /* generic template */
    parse_struct_body(find_struct(n), line);
}
static void parse_struct_body(StructDef *sd, int line) {
    char *n = sd->name;
    if (sd->done) die(line, "struct '%s' redefined", n);
    expectop("{"); int off = 0, al = 1;
    while (!isop("}")) {
        Field f; memset(&f, 0, sizeof f); f.is_mut = 1; int fl = cur()->line; f.name = expect_id(); expectop(":"); f.ty = parse_type(); need_complete(f.ty, fl);
        if (has_iface(f.ty)) die(fl, IFACE_MSG, tname(f.ty));
        if (!acceptop(";")) acceptop(",");
        if (f.ty->align > al) al = f.ty->align;
        off = (off + f.ty->align - 1) / f.ty->align * f.ty->align; f.off = off; off += f.ty->size;
        for (int i = 0; i < sd->nf; i++) if (!strcmp(sd->f[i].name, f.name)) die(fl, "duplicate field '%s'", f.name);
        VPUSH(sd->f, sd->nf, sd->cf, f);
    }
    tp++; sd->ty->size = (off + al - 1) / al * al; sd->ty->align = al; if (!sd->ty->size) sd->ty->size = 1; sd->done = 1;
}
static void parse_enum(void) {
    char *n = expect_id(); EnumDef *ed = find_enum(n); expectop("{");
    int pl = 0;   /* largest payload */
    while (!isop("}")) {
        int vline = cur()->line; char *vn = expect_id();
        for (int i = 0; i < ed->nv; i++) if (!strcmp(ed->vn[i], vn)) die(vline, "duplicate variant '%s' in enum '%s'", vn, n);
        VPUSH(ed->vn, ed->nv, ed->cv, vn);
        VarInfo vi; memset(&vi, 0, sizeof vi);
        if (acceptop("(")) {   /* Variant(T1, T2, ...): the values it carries, laid out like struct fields after the tag */
            int off = 8;
            if (isop(")")) die(vline, "variant '%s' needs at least one value type (or no parentheses)", vn);
            do {
                int fl = cur()->line;
                if (vi.nf >= MAXVF) die(fl, "too many values in variant '%s' (at most %d)", vn, MAXVF);
                Type *ft = parse_type(); need_complete(ft, fl);
                if (ft->k == TY_VOID) die(fl, "a variant cannot hold 'void'");
                if (has_iface(ft)) die(fl, IFACE_MSG, tname(ft));
                off = (off + ft->align - 1) / ft->align * ft->align; vi.ft[vi.nf] = ft; vi.off[vi.nf] = off; vi.nf++; off += ft->size;
            } while (acceptop(","));
            expectop(")");
            if (off - 8 > pl) pl = off - 8;
        }
        VPUSH(ed->vi, ed->nvi, ed->cvi, vi);
        if (!acceptop(",")) break;
    }
    expectop("}");
    if (ed->data) { ed->ty->size = 8 + (pl + 7) / 8 * 8; ed->ty->align = 8; }   /* tag + the largest payload */
    ed->done = 1;
}
static const char *vis_name(int v) { static const char *n[] = { "public", "private", "protected", "internal" }; return n[v]; }
static int vis_of(const char *s) { for (int i = 0; i < 4; i++) if (!strcmp(vis_name(i), s)) return i; return -1; }
static void parse_fn_ex(StructDef *cls, int is_unsafe, int is_extern, int is_static, int vis, int is_ctor, char *force, Iface *ifc) {
    Func *f = xalloc(sizeof *f); f->line = cur()->line; f->is_unsafe = is_unsafe; f->is_extern = is_extern;
    f->borrow = rt_fidx >= 0 && (f->line >> 26) == rt_fidx;
    char *mn = is_ctor == 2 ? "dtor" : is_ctor ? "ctor" : expect_id();
    int is_dtor = is_ctor == 2; if (is_dtor) is_ctor = 0;
    if (force) { skip_type_params(); mn = force; }
    if (cls || ifc) { char buf[300]; snprintf(buf, sizeof buf, "%s.%s", cls ? cls->name : ifc->name, mn); f->name = xstrn(buf, strlen(buf)); } else f->name = mn;
    f->cls = cls; f->is_ctor = is_ctor; f->is_static = is_static; f->vis = vis; f->is_method = cls && !is_static && !is_ctor && !is_dtor; f->is_dtor = is_dtor;
    if (is_dtor) cls->dtor = f;
    f->pname = xalloc(MAXPARAMS * sizeof(char *)); f->pty = xalloc(MAXPARAMS * sizeof(Type *)); f->pref = xalloc(MAXPARAMS * sizeof(int));
    if (cls && !is_static) { f->pname[0] = "this"; f->pty[0] = cls->ty; f->pref[0] = 1; f->nparams = 1; }
    if (ifc) { f->ifc = ifc; f->is_method = 1; f->vt_idx = ifc->nm; f->pname[0] = "this"; f->pty[0] = ifc->ty; f->pref[0] = 1; f->nparams = 1; }
    if (is_dtor && !isop("(")) goto params_done;   /* dtor { ... } */
    expectop("(");
    if (!isop(")")) do {
        if (f->nparams >= MAXPARAMS) die(cur()->line, "too many parameters");
        int ref = acceptkw("ref"); char *pn = expect_id(); expectop(":"); Type *pt = parse_type();
        if (has_iface(pt) && (ref || pt->k != TY_IFACE)) die(f->line, IFACE_MSG, tname(pt));
        f->pname[f->nparams] = pn; f->pty[f->nparams] = pt; f->pref[f->nparams] = ref; f->nparams++;
    } while (acceptop(","));
    expectop(")");
    if (is_dtor && f->nparams != 1) die(f->line, "a destructor takes no parameters");
params_done:
    f->ret = (!is_ctor && !is_dtor && acceptop("->")) ? parse_type() : &ty_void;
    if (acceptkw("requires")) do {   /* callers need these capabilities */
        if (f->nreq == 8) die(f->line, "a function can require at most 8 capabilities");
        f->req[f->nreq++] = parse_dotted();
    } while (acceptop(","));
    if (has_iface(f->ret)) die(f->line, IFACE_MSG, tname(f->ret));
    for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, f->name)) {
        Func *g = funcs[i]; int same_sig = is_extern && g->is_extern && g->nparams == f->nparams && same(g->ret, f->ret);
        for (int k = 0; same_sig && k < f->nparams; k++) if (g->pref[k] != f->pref[k] || !same(g->pty[k], f->pty[k])) same_sig = 0;
        if (same_sig) { expectop(";"); return; }   /* the same extern declared again (e.g. by two modules) */
        if (rt_fidx >= 0 && (f->line >> 26) == rt_fidx) die(g->line, "'%s' is a runtime function (lib/rt/prelude.svl); declare it with the same signature or use another name", f->name);
        die(f->line, "function '%s' redefined", f->name);
    }
    if (is_extern || ifc) { expectop(";"); } else f->body = parse_block();
    if (ifc) VPUSH(ifc->m, ifc->nm, ifc->cm, f);
    VPUSH(funcs, nfuncs, cfuncs, f);
}
static void parse_fn(int is_unsafe, int is_extern) {
    if (cur()->k == TK_ID && toks[tp + 1].k == TK_OP && !strcmp(toks[tp + 1].s, "<")) {   /* generic template: skipped until instantiated */
        tp += 2; skip_type_params();
        while (!isop("{")) { if (cur()->k == TK_EOF) die(cur()->line, "unexpected end of file"); tp++; }
        skip_block(); return;
    }
    parse_fn_ex(0, is_unsafe, is_extern, 0, 0, 0, 0, 0);
}

/* class bodies are parsed in two passes: fields first (so the layout is known and the class is complete), then methods */
static void skip_member(void) {
    int depth = 0;
    for (;;) {
        if (cur()->k == TK_EOF) die(cur()->line, "unexpected end of file in class");
        if (isop("{")) depth++;
        else if (isop("}")) { depth--; if (depth <= 0) { tp++; return; } }
        else if (isop(";") && depth == 0) { tp++; return; }
        tp++;
    }
}
static void parse_class_pass(StructDef *sd, int pass, int *has_ctor) {
    int vis = 1, gstatic = 0, off = 0, al = 1;
    if (sd->base) { off = sd->base->ty->size; al = sd->base->ty->align; }
    while (!isop("}")) {
        int ml = cur()->line;
        if (cur()->k == TK_EOF) die(ml, "unexpected end of file in class");
        if (cur()->k == TK_ID && vis_of(cur()->s) >= 0 && ((toks[tp + 1].k == TK_OP && !strcmp(toks[tp + 1].s, ":")) ||
            (toks[tp + 1].k == TK_ID && !strcmp(toks[tp + 1].s, "static") && toks[tp + 2].k == TK_OP && !strcmp(toks[tp + 2].s, ":")))) {
            vis = vis_of(cur()->s); tp++; gstatic = 0; if (acceptkw("static")) gstatic = 1; expectop(":"); continue;
        }
        if (iskw("let") || iskw("var")) {
            if (pass == 1) { skip_member(); continue; }
            Field f; memset(&f, 0, sizeof f); f.is_mut = iskw("var"); f.vis = vis; f.owner = sd; tp++; f.name = expect_id(); expectop(":"); f.ty = parse_type(); need_complete(f.ty, ml);
            if (has_iface(f.ty)) die(ml, IFACE_MSG, tname(f.ty));
            if (acceptop("=")) f.init = parse_expr();
            expectop(";");
            if (f.ty->align > al) al = f.ty->align;
            off = (off + f.ty->align - 1) / f.ty->align * f.ty->align; f.off = off; off += f.ty->size;
            for (int i = 0; i < sd->nf; i++) if (!strcmp(sd->f[i].name, f.name)) die(ml, "duplicate field '%s'", f.name);
            VPUSH(sd->f, sd->nf, sd->cf, f);
            continue;
        }
        int st = gstatic;
        if (iskw("static") && !(toks[tp + 1].k == TK_OP)) { tp++; st = 1; }
        if (pass == 0) { skip_member(); continue; }
        if (iskw("ctor")) { tp++; *has_ctor = 1; parse_fn_ex(sd, 0, 0, 0, 0, 1, 0, 0); }
        else if (iskw("dtor")) { tp++; parse_fn_ex(sd, 0, 0, 0, 0, 2, 0, 0); }
        else if (acceptkw("fn")) parse_fn_ex(sd, 0, 0, st, vis, 0, 0, 0);
        else if (acceptkw("unsafe")) { if (!acceptkw("fn")) die(ml, "expected 'fn' after 'unsafe'"); parse_fn_ex(sd, 1, 0, st, vis, 0, 0, 0); }
        else if (acceptkw("safe")) { if (!acceptkw("fn")) die(ml, "expected 'fn' after 'safe'"); parse_fn_ex(sd, 0, 0, st, vis, 0, 0, 0); }
        else die(ml, "unexpected '%s' in class body (expected a field, 'fn' or 'ctor')", cur()->s ? cur()->s : "token");
    }
    if (pass == 0) { sd->ty->size = (off + al - 1) / al * al; sd->ty->align = al; if (!sd->ty->size) sd->ty->size = 1; }
}
static void parse_class(void) {
    int line = cur()->line; char *n = expect_id();
    if (isop("<")) { skip_type_params(); while (!isop("{")) { if (cur()->k == TK_EOF) die(line, "unexpected end of file"); tp++; } skip_block(); return; }   /* generic template */
    parse_class_body(find_struct(n), line);
}
static void parse_class_body(StructDef *sd, int line) {
    char *n = sd->name;
    if (sd->done) die(line, "class '%s' redefined", n);
    sd->line = line;
    if (acceptop(":")) do {   /* class C : Base, Iface1, Iface2 */
        int bl = cur()->line; Type *bt = parse_type();
        if (bt->k == TY_IFACE) { VPUSH(sd->ifs, sd->nifs, sd->cifs, bt->ifc); }
        else if (bt->k == TY_STRUCT && bt->sd->is_class) {
            if (sd->base) die(bl, "class '%s' can only extend one class", n);
            if (sd->nifs) die(bl, "the base class must come before the interfaces");
            need_complete(bt, bl); if (is_sub(bt->sd, sd)) die(bl, "class '%s' cannot extend itself", n);
            sd->base = bt->sd;
            for (int i = 0; i < bt->sd->nf; i++) VPUSH(sd->f, sd->nf, sd->cf, bt->sd->f[i]);   /* inherited fields: same offsets */
        } else die(bl, "'%s' is not a class or interface", tname(bt));
    } while (acceptop(","));
    expectop("{"); int start = tp, has_ctor = 0;
    parse_class_pass(sd, 0, &has_ctor);
    sd->done = 1; sd->is_class = 1;
    tp = start; parse_class_pass(sd, 1, &has_ctor);
    expectop("}");
    if (!has_ctor) {   /* synthesize an empty constructor so field defaults still run */
        Func *f = xalloc(sizeof *f); f->line = line; char buf[300]; snprintf(buf, sizeof buf, "%s.ctor", sd->name); f->name = xstrn(buf, strlen(buf));
        f->cls = sd; f->is_ctor = 1; f->ret = &ty_void;
        f->pname = xalloc(MAXPARAMS * sizeof(char *)); f->pty = xalloc(MAXPARAMS * sizeof(Type *)); f->pref = xalloc(MAXPARAMS * sizeof(int));
        f->pname[0] = "this"; f->pty[0] = sd->ty; f->pref[0] = 1; f->nparams = 1;
        f->body = mks(S_BLOCK, line); VPUSH(funcs, nfuncs, cfuncs, f);
    }
}
static void parse_interface(void) {
    int line = cur()->line; char *n = expect_id(); Iface *ifc = find_iface(n);
    if (ifc->nm) die(line, "interface '%s' redefined", n);
    expectop("{");
    while (!isop("}")) {
        int ml = cur()->line;
        if (cur()->k == TK_EOF) die(ml, "unexpected end of file in interface");
        if (acceptkw("fn")) parse_fn_ex(0, 0, 0, 0, 0, 0, 0, ifc);
        else if (acceptkw("unsafe")) { if (!acceptkw("fn")) die(ml, "expected 'fn' after 'unsafe'"); parse_fn_ex(0, 1, 0, 0, 0, 0, 0, ifc); }
        else die(ml, "interfaces can only contain method signatures ('fn name(..) -> T;')");
    }
    tp++;
}
static void prescan(void) {
    for (int i = 0; toks[i].k != TK_EOF; i++) {
        if (toks[i].k == TK_ID && toks[i + 1].k == TK_ID && toks[i + 2].k == TK_OP && !strcmp(toks[i + 2].s, "<") &&
            (!strcmp(toks[i].s, "class") || !strcmp(toks[i].s, "struct") || !strcmp(toks[i].s, "fn"))) {
            Generic *g = xalloc(sizeof *g); g->name = toks[i + 1].s; g->line = toks[i].line;
            g->kind = toks[i].s[0] == 'c' ? 0 : toks[i].s[0] == 's' ? 1 : 2;
            g->is_unsafe = i > 0 && toks[i - 1].k == TK_ID && !strcmp(toks[i - 1].s, "unsafe");
            g->is_async = i > 0 && toks[i - 1].k == TK_ID && !strcmp(toks[i - 1].s, "async");
            int j = i + 3;
            while (toks[j].k == TK_ID) {
                if (g->np >= 8) die(g->line, "too many type parameters");
                g->pn[g->np] = toks[j].s; j++;
                if (toks[j].k == TK_OP && !strcmp(toks[j].s, ":") && toks[j + 1].k == TK_ID) { g->pc[g->np] = toks[j + 1].s; j += 2; }
                g->np++;
                if (toks[j].k == TK_OP && !strcmp(toks[j].s, ",")) j++; else break;
            }
            if (!g->np || !(toks[j].k == TK_OP && !strcmp(toks[j].s, ">"))) die(g->line, "malformed type parameter list");
            g->start = g->kind == 2 ? i + 1 : j + 1;
            if (find_generic(g->name) || find_struct(g->name) || find_enum(g->name) || find_iface(g->name)) die(g->line, "type '%s' redefined", g->name);
            VPUSH(gens, ngens, cgens, g); continue;
        }
        if (toks[i].k == TK_ID && toks[i + 1].k == TK_ID && !strcmp(toks[i].s, "interface")) {
            char *n = toks[i + 1].s;
            if (find_struct(n) || find_enum(n) || find_generic(n) || find_iface(n)) die(toks[i].line, "type '%s' redefined", n);
            Iface *ifc = xalloc(sizeof *ifc); ifc->name = n; ifc->ty = xalloc(sizeof(Type)); ifc->ty->k = TY_IFACE; ifc->ty->ifc = ifc; ifc->ty->size = 16; ifc->ty->align = 8;
            VPUSH(ifaces, nifaces, cifaces, ifc); continue;
        }
        if (toks[i].k == TK_ID && toks[i + 1].k == TK_ID && (!strcmp(toks[i].s, "struct") || !strcmp(toks[i].s, "enum") || !strcmp(toks[i].s, "class"))) {
            char *n = toks[i + 1].s;
            if (find_struct(n) || find_enum(n) || find_generic(n) || find_iface(n)) die(toks[i].line, "type '%s' redefined", n);
            if (toks[i].s[0] != 'e') { StructDef *sd = xalloc(sizeof *sd); sd->name = n; sd->ty = xalloc(sizeof(Type)); sd->ty->k = TY_STRUCT; sd->ty->sd = sd; sd->ty->size = 1; sd->ty->align = 1; VPUSH(structs, nstructs, cstructs, sd); }
            else {
                EnumDef *ed = xalloc(sizeof *ed); ed->name = n; ed->ty = xalloc(sizeof(Type)); ed->ty->k = TY_ENUM; ed->ty->size = ed->ty->align = 4; ed->ty->sign = 1; ed->ty->ed = ed; VPUSH(enums, nenums, cenums, ed);
                if (toks[i + 2].k == TK_OP && !strcmp(toks[i + 2].s, "{"))   /* a '(' in the body: variants carry values */
                    for (int j = i + 3; toks[j].k != TK_EOF && !(toks[j].k == TK_OP && !strcmp(toks[j].s, "}")); j++)
                        if (toks[j].k == TK_OP && !strcmp(toks[j].s, "(")) { ed->data = 1; ed->ty->k = TY_DATA; ed->ty->size = ed->ty->align = 8; ed->ty->sign = 0; break; }
            }
        }
    }
}
static void parse_program(void) {
    while (cur()->k != TK_EOF) {
        int line = cur()->line, imp = POS_FILE(line) != 0;
        if (imp && (iskw("allow") || iskw("deny") || iskw("protect"))) die(line, "imported modules cannot declare capabilities or protection; the importing module decides");
        if (acceptkw("module")) { char *mn = parse_dotted(); if (!imp) mod_name = mn; expectop(";"); }
        else if (acceptkw("import")) { parse_dotted(); expectop(";"); }
        else if (acceptkw("allow")) { char *c = parse_dotted(); expectop(";"); VPUSH(allow_caps, nallow, callow, c); }
        else if (acceptkw("deny")) { char *c = parse_dotted(); expectop(";"); VPUSH(deny_caps, ndeny, cdeny, c); }
        else if (acceptkw("protect")) { char *c = expect_id(); if (strcmp(c, "Process")) die(line, "only 'protect Process;' is supported"); protect_process = 1; expectop(";"); }
        else if (acceptkw("const")) {
            Local *g = xalloc(sizeof *g); g->is_global = 1; g->is_const = 1; g->line = line; g->name = expect_id();
            if (acceptop(":")) g->ty = parse_type();
            expectop("="); Expr *x = parse_expr(); long long v;
            if (const_eval(x, &v)) { int cl = x->line; x = mk(E_INT, cl); x->ival = v; x->neg = v < 0; }
            else if (!(x->k == E_FLOAT || x->k == E_BOOL || x->k == E_STR)) die(line, "constant '%s' must be a literal or an integer expression of literals and constants", g->name);
            g->init = x; expectop(";"); VPUSH(globals, nglobals, cglobals, g);
        }
        else if (iskw("let") || iskw("var")) {
            Local *g = xalloc(sizeof *g); g->is_global = 1; g->is_mut = iskw("var"); g->line = line; tp++; g->name = expect_id();
            if (acceptop(":")) g->ty = parse_type();
            if (acceptop("=")) g->init = parse_expr();
            if (!g->ty && !g->init) die(line, "global '%s' needs a type or an initializer", g->name);
            expectop(";"); VPUSH(globals, nglobals, cglobals, g);
        }
        else if (acceptkw("struct")) parse_struct();
        else if (acceptkw("enum")) parse_enum();
        else if (acceptkw("class")) parse_class();
        else if (acceptkw("interface")) parse_interface();
        else if (acceptkw("extern")) { if (!acceptkw("fn")) die(line, "expected 'fn' after 'extern'"); parse_fn(1, 1); }
        else if (acceptkw("unsafe")) { if (!acceptkw("fn")) die(line, "expected 'fn' after 'unsafe'"); parse_fn(1, 0); }
        else if (acceptkw("safe")) {
            if (acceptkw("extern")) {   /* a C function the standard library vouches for: callable from safe code */
                if (!file_trusted[POS_FILE(line)]) die(line, "only the standard library can declare 'safe extern' functions");
                if (!acceptkw("fn")) die(line, "expected 'fn' after 'extern'"); parse_fn(0, 1);
            } else { if (!acceptkw("fn")) die(line, "expected 'fn' after 'safe'"); parse_fn(0, 0); }
        }
        else if (acceptkw("fn")) parse_fn(0, 0);
        else if (isop("@")) {   /* attributes: @test, @deprecated before a function */
            int t = 0, d = 0;
            while (acceptop("@")) {
                int al = cur()->line; char *a = expect_id();
                if (!strcmp(a, "test")) t = 1; else if (!strcmp(a, "deprecated")) d = 1;
                else die(al, "unknown attribute '@%s' (known: @test, @deprecated)", a);
            }
            int nf0 = nfuncs;
            if (!acceptkw("fn")) die(cur()->line, "an attribute must be followed by a function");
            parse_fn(0, 0);
            if (nfuncs == nf0) die(line, "attributes cannot be used on generic functions");
            funcs[nfuncs - 1]->is_test = t; funcs[nfuncs - 1]->is_deprecated = d;
        }
        else if (acceptkw("async")) {   /* async fn: calling it starts a task */
            if (!acceptkw("fn")) die(line, "expected 'fn' after 'async'");
            int nf0 = nfuncs; parse_fn(0, 0);
            if (nfuncs > nf0) funcs[nfuncs - 1]->is_async = 1;
        }
        else die(line, "unexpected '%s' at top level (expected fn, struct, class, enum, interface, const, let, var or import)", cur()->s ? cur()->s : "token");
    }
}

/* ============================ semantic analysis ============================ */
static Local **locals; static int nlocals, clocals, scope_base;
static Func *cur_fn; static int in_unsafe, cur_off, max_off, loop_depth;

static int cap_matches(const char *rule, const char *c) { size_t n = strlen(rule); return !strcmp(rule, c) || (!strncmp(rule, c, n) && c[n] == '.'); }
static int cap_ok(const char *c) {
    for (int i = 0; i < ndeny; i++) if (cap_matches(deny_caps[i], c)) return 0;
    for (int i = 0; i < nallow; i++) if (cap_matches(allow_caps[i], c)) return 1;
    return 0;
}
static void need_cap(const char *c, const char *what, int line) {
    if (file_trusted[POS_FILE(line)]) return;
    for (int i = 0; i < ndeny; i++) if (cap_matches(deny_caps[i], c)) die(line, "%s requires capability '%s', which this module explicitly denies", what, c);
    if (!cap_ok(c)) die(line, "%s requires capability '%s' (add 'allow %s;' to the module)", what, c, c);
}
/* ---- extern function classification: what capability does calling this libc function need?
 * Plain-Native functions are harmless helpers. Dangerous ones map to a specific capability.
 * Anything not listed needs `allow Unrestricted;` (deny by default). */
static const char *safe_externs[] = { "malloc", "calloc", "realloc", "free", "memcpy", "memmove", "memset", "memcmp",
    "strlen", "strcmp", "strncmp", "strchr", "strcpy", "abort", "exit", "printf", "sprintf", "snprintf", "puts", "putchar",
    "fflush", "fclose", "fseek", "ftell", "getpid", "sleep", "usleep", "abs", "atoi", "rand", "srand", "time", "clock",
    "waitpid", "__errno_location", "fprintf",
    "sqrt", "sin", "cos", "tan", "atan", "atan2", "pow", "exp", "log", "log10", "floor", "ceil", "fabs", "fmod", "round", "strtod", "strtof", "strstr", "sscanf", "strfromd", "strfromf", "open_memstream",
    "dprintf", "setjmp", "longjmp", 0 };
static const struct { const char *fn, *cap; } cap_externs[] = {
    { "ptrace", "Process.Debug" }, { "process_vm_readv", "Process.MemoryRead" }, { "process_vm_writev", "Process.MemoryWrite" },
    { "fopen", "File.Read" }, { "fread", "File.Read" }, { "open", "File.Read" }, { "fgets", "File.Read" },
    { "fwrite", "File.Write" }, { "remove", "File.Write" }, { "unlink", "File.Write" }, { "rename", "File.Write" }, { "mkdir", "File.Write" },
    { "socket", "Network" }, { "connect", "Network" }, { "bind", "Network" }, { "listen", "Network" }, { "accept", "Network" },
    { "send", "Network" }, { "recv", "Network" },
    { "fork", "Process.Spawn" }, { "execve", "Process.Spawn" }, { "execvp", "Process.Spawn" }, { "system", "Process.Spawn" },
    { "posix_spawn", "Process.Spawn" }, { "kill", "Process.Signal" },
    { "dlopen", "Process.Inject" }, { "dlsym", "Unrestricted" }, { "syscall", "Unrestricted" }, { 0, 0 } };
static const char *extern_cap(const char *fn, int *known) {
    *known = 1;
    for (int i = 0; safe_externs[i]; i++) if (!strcmp(safe_externs[i], fn)) return 0;
    for (int i = 0; cap_externs[i].fn; i++) if (!strcmp(cap_externs[i].fn, fn)) return cap_externs[i].cap;
    *known = 0; return "Unrestricted";
}
static Local *lookup(const char *n) {
    for (int i = nlocals - 1; i >= 0; i--) if (!strcmp(locals[i]->name, n)) return locals[i];
    for (int i = 0; i < nglobals; i++) if (!strcmp(globals[i]->name, n)) return globals[i];
    return 0;
}
static int alloc_slot(int size, int align) { cur_off = (cur_off + size + align - 1) / align * align; if (cur_off > max_off) max_off = cur_off; return cur_off; }
static Local *declare(const char *n, Type *t, int is_mut, int is_ref, int line) {
    for (int i = scope_base; i < nlocals; i++) if (!strcmp(locals[i]->name, n)) die(line, "'%s' already declared in this scope", n);
    Local *l = xalloc(sizeof *l); l->name = (char *)n; l->ty = t; l->is_mut = is_mut; l->is_ref = is_ref;
    l->off = is_ref ? alloc_slot(8, 8) : alloc_slot(t->size, t->align);
    l->loop = loop_depth;
    if (!is_ref && has_drop(t)) l->dflag = alloc_slot(8, 8);
    VPUSH(locals, nlocals, clocals, l); return l;
}
/* ---- ownership: values with destructors are moved, never copied ---- */
static Stmt *cur_stmt; static int assign_target; static char ctor_set[256]; static int uses_async;
static void consume(Expr *e) {   /* e's value is taken over by a new owner */
    if (!e->ty || !has_drop(e->ty)) return;
    if (!move_only(e->ty)) {   /* reference-counted values: reading a place copies it, a fresh value is handed over */
        switch (e->k) {
        case E_VAR: case E_FIELD: case E_INDEX: e->copy = 1; return;
        case E_UN: if (e->op == O_DEREF) { e->copy = 1; return; } break;
        case E_CAST: consume(e->a); return;   /* String -> String, or a raw pointer inside the runtime */
        case E_STR: return;   /* literals are immortal */
        default: e->consumed = 1; return;
        }
    }
    switch (e->k) {
    case E_VAR:
        if (e->lv->is_global) die(e->line, "cannot move out of global '%s'", e->name);
        if (e->lv->is_ref) die(e->line, "cannot move out of '%s' (it is borrowed with 'ref')", e->name);
        if (e->lv->loop < loop_depth) die(e->line, "cannot move '%s' inside a loop (it is declared outside the loop)", e->name);
        e->lv->moved = 1; e->move = 1; return;
    case E_CALL: case E_OK: case E_ERR: case E_TRY: case E_AWAIT: case E_SLIT: case E_VARIANT: e->consumed = 1; return;
    case E_UN: if (e->op == O_DEREF) return;   /* raw read through a pointer (unsafe): the programmer owns it */
        /* fallthrough */
    default: die(e->line, "cannot move out of this place: '%s' has a destructor (use 'ref', or a method that takes the value out)", tname(e->ty));
    }
}
static void register_temp(Expr *e) {   /* a temporary that is dropped at the end of its statement unless consumed */
    if (!cur_stmt) die(e->line, "this expression creates a temporary with a destructor; assign it to a variable first");
    e->tnext = cur_stmt->temps; cur_stmt->temps = e;
}
static void close_temps(Stmt *s) {
    Expr **pp = &s->temps;
    while (*pp) {
        Expr *t = *pp;
        if (t->consumed) { *pp = t->tnext; continue; }
        if (t->k == E_TRY) die(t->line, "the value of '?' must be used here (it has a destructor)");
        if (!t->dflag) t->dflag = alloc_slot(8, 8);
        pp = &t->tnext;
    }
}
static char *snap_moved(void) { char *m = xalloc((size_t)nlocals + 1); for (int i = 0; i < nlocals; i++) m[i] = (char)locals[i]->moved; return m; }
static void set_moved(char *m, int n) { for (int i = 0; i < n; i++) locals[i]->moved = m[i]; }
static void require_safe_type(Type *t, int line) { if (has_ptr(t) && !in_unsafe) die(line, "pointer types are only allowed in unsafe code"); }

static void range_check(Expr *e, Type *t) {
    long long v = e->ival;
    if (t->size == 8) { if (!t->sign && e->neg && v != 0) die(e->line, "negative literal for unsigned type '%s'", tname(t)); return; }
    long long lo, hi;
    if (t->sign) { hi = (1LL << (t->size * 8 - 1)) - 1; lo = -hi - 1; } else { lo = 0; hi = (1LL << (t->size * 8)) - 1; }
    if (v < lo || v > hi) die(e->line, "literal %lld out of range for '%s'", v, tname(t));
}
static void retype(Expr *e, Type *t) {   /* give an integer/float literal expression its final type */
    if (e->ty->k != TY_ILIT && e->ty->k != TY_FLIT) return;
    switch (e->k) {
    case E_INT: if (is_int(t)) range_check(e, t); break;
    case E_BIN: retype(e->a, t); retype(e->b, t); break;
    case E_UN: retype(e->a, t); break;
    default: break;
    }
    e->ty = t;
}
static int alloc_slot(int size, int align); static int get_vtable(StructDef *sd, Iface *ifc);
static int coerce_to(Expr *e, Type *t) {
    if (e->ty->k == TY_RESPEND) {   /* Ok(..) / Err(..) / Some(..) / None take their type from the context */
        if (t->k != TY_RESULT || t->opt != e->ty->opt) return 0;
        Type *pt = e->k == E_ERR ? t->err : t->elem;
        if (e->a) { if (!coerce_to(e->a, pt)) return 0; } else if (pt->k != TY_VOID) return 0;
        e->ty = t; e->tmp = alloc_slot(t->size, 8);
        if (e->a) consume(e->a);
        if (has_drop(t)) register_temp(e);
        return 1;
    }
    if (e->ty->k == TY_NULL) { if (t->k == TY_PTR || t->k == TY_STR) { e->ty = t; return 1; } return 0; }
    if (e->ty->k == TY_ILIT) { if (t->k != TY_INT && t->k != TY_FLOAT) return 0; retype(e, t); return 1; }
    if (e->ty->k == TY_FLIT) { if (t->k != TY_FLOAT) return 0; retype(e, t); return 1; }
    if (t->k == TY_STRUCT && e->ty->k == TY_STRUCT && e->ty->sd != t->sd && is_sub(e->ty->sd, t->sd)) return !has_drop(e->ty);   /* upcast: copies the base part */
    if (t->k == TY_IFACE && e->ty->k == TY_STRUCT && e->ty->sd->is_class && implements(e->ty->sd, t->ifc)) {
        int vt = get_vtable(e->ty->sd, t->ifc);   /* borrow: {address of the object, vtable} */
        Expr *in = xalloc(sizeof *in); *in = *e; memset(e, 0, sizeof *e);
        e->k = E_TOIFACE; e->line = in->line; e->a = in; e->ty = t; e->ival = vt; e->tmp = alloc_slot(16, 8);
        return 1;
    }
    return same(e->ty, t);
}
static void finalize(Expr *e) {
    if (e->ty && e->ty->k == TY_ILIT) retype(e, &ty_i32);
    if (e->ty && e->ty->k == TY_FLIT) retype(e, &ty_f64);
    if (e->ty && e->ty->k == TY_RESPEND && e->ty->opt) die(e->line, "cannot infer the Option type of '%s'; use it where an Option is expected", e->k == E_ERR ? "None" : "Some(...)");
    if (e->ty && e->ty->k == TY_RESPEND) die(e->line, "cannot infer the Result type of '%s(...)'; use it where a Result is expected", e->k == E_ERR ? "Err" : "Ok");
}
static void to_flit(Expr *e) {   /* an integer literal expression used with a float literal becomes a float literal */
    if (e->ty->k != TY_ILIT) return;
    if (e->k == E_BIN) { to_flit(e->a); to_flit(e->b); } else if (e->k == E_UN) to_flit(e->a);
    e->ty = &ty_flit;
}
static void unify(Expr *a, Expr *b) {
    if (a->ty->k == TY_ILIT && is_int(b->ty)) retype(a, b->ty);
    else if (b->ty->k == TY_ILIT && is_int(a->ty)) retype(b, a->ty);
    else if ((a->ty->k == TY_ILIT || a->ty->k == TY_FLIT) && is_float(b->ty)) retype(a, b->ty);
    else if ((b->ty->k == TY_ILIT || b->ty->k == TY_FLIT) && is_float(a->ty)) retype(b, a->ty);
    else if (a->ty->k == TY_ILIT && b->ty->k == TY_FLIT) to_flit(a);
    else if (b->ty->k == TY_ILIT && a->ty->k == TY_FLIT) to_flit(b);
}
static void expect_coerce(Expr *e, Type *t, const char *ctx) {
    if (!coerce_to(e, t)) die(e->line, "%s: expected '%s', found '%s'", ctx, tname(t), tname(e->ty));
}

static Type *check_expr(Expr *e);
static int can_access(int vis, StructDef *owner) {   /* private: same class; protected: same class or a subclass */
    if (vis == 1) return cur_fn && cur_fn->cls == owner;
    if (vis == 2) return cur_fn && cur_fn->cls && is_sub(cur_fn->cls, owner);
    return 1;
}
static Func *find_method(StructDef *sd, const char *m) {   /* instance method, searching base classes */
    for (StructDef *c = sd; c; c = c->base) {
        char full[300]; snprintf(full, sizeof full, "%s.%s", c->name, m);
        for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, full) && !funcs[i]->is_ctor && !funcs[i]->is_static && !funcs[i]->is_dtor) return funcs[i];
    }
    return 0;
}
static void check_impl(StructDef *sd, Iface *ifc, int line) {
    for (int j = 0; j < ifc->nm; j++) {
        Func *im = ifc->m[j]; const char *mn = strrchr(im->name, '.') + 1;
        Func *cm = find_method(sd, mn);
        if (!cm) die(line, "class '%s' does not implement '%s' from interface '%s'", sd->name, mn, ifc->name);
        int ok = cm->nparams == im->nparams && same(cm->ret, im->ret);
        for (int k = 1; ok && k < im->nparams; k++) ok = same(cm->pty[k], im->pty[k]) && cm->pref[k] == im->pref[k];
        if (!ok) die(line, "'%s' does not match the signature of '%s'", cm->name, im->name);
        if (cm->vis != 0) die(line, "'%s' must be public to implement interface '%s'", cm->name, ifc->name);
    }
}
typedef struct { StructDef *sd; Iface *ifc; } VTab;
static VTab *vts; static int nvts, cvts;
static int get_vtable(StructDef *sd, Iface *ifc) {
    for (int i = 0; i < nvts; i++) if (vts[i].sd == sd && vts[i].ifc == ifc) return i;
    check_impl(sd, ifc, sd->line);
    VTab v = { sd, ifc }; VPUSH(vts, nvts, cvts, v); return nvts - 1;
}
/* bind type parameters by matching a parameter type pattern against an argument type */
static void infer_unify(Type *pat, Type *act, Type **bound, int lit_ok) {
    if (pat->k == TY_PARAM) {
        if (act->k == TY_ILIT || act->k == TY_NULL || act->k == TY_FLIT) {
            if (lit_ok && !bound[pat->n] && act->k == TY_ILIT) bound[pat->n] = &ty_i32;
            if (lit_ok && !bound[pat->n] && act->k == TY_FLIT) bound[pat->n] = &ty_f64;
            return;
        }
        if (!bound[pat->n]) bound[pat->n] = act;
        return;
    }
    if (pat->k != act->k) return;
    if (pat->k == TY_STRUCT) { if (pat->sd->gen && pat->sd->gen == act->sd->gen) for (int i = 0; i < pat->sd->gen->np; i++) infer_unify(pat->sd->targs[i], act->sd->targs[i], bound, lit_ok); return; }
    if (pat->k == TY_PTR || pat->k == TY_ARRAY) infer_unify(pat->elem, act->elem, bound, lit_ok);
    else if (pat->k == TY_RESULT) { infer_unify(pat->elem, act->elem, bound, lit_ok); infer_unify(pat->err, act->err, bound, lit_ok); }
}
static Func *infer_generic(Generic *g, Expr *e) {
    Type *ph[8], *bound[8] = { 0 };
    for (int i = 0; i < g->np; i++) { ph[i] = xalloc(sizeof(Type)); ph[i]->k = TY_PARAM; ph[i]->n = i; ph[i]->size = ph[i]->align = 8; }
    PState ps; ps_save(&ps); ps_bind(g, ph); tp = g->start;
    expect_id(); skip_type_params(); expectop("(");
    Type *pt[16]; int pr[16], np = 0;
    if (!isop(")")) do { if (np >= 16) die(g->line, "too many parameters"); pr[np] = acceptkw("ref"); expect_id(); expectop(":"); pt[np++] = parse_type(); } while (acceptop(","));
    ps_restore(&ps);
    if (np != e->nargs) die(e->line, "'%s' expects %d argument(s), got %d", g->name, np, e->nargs);
    for (int i = 0; i < np; i++) { Expr *a = e->args[i]; check_expr(a->k == E_REF ? a->a : a); }
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < np; i++) { Expr *a = e->args[i]; infer_unify(pt[i], (a->k == E_REF ? a->a : a)->ty, bound, pass); }
    for (int i = 0; i < g->np; i++) if (!bound[i]) die(e->line, "cannot infer type parameter '%s' of '%s'; call it as %s<...>(...)", g->pn[i], g->name, g->name);
    return instantiate_fn(g, bound, e->line);
}
static int is_lvalue(Expr *e) {
    if (e->k == E_INDEX && e->a->ty && e->a->ty->k == TY_STR) return 0;   /* strings are immutable */
    return e->k == E_VAR || e->k == E_INDEX || e->k == E_FIELD || (e->k == E_UN && e->op == O_DEREF);
}
static int root_mutable(Expr *e) {
    switch (e->k) {
    case E_VAR: return e->lv->is_mut;
    case E_INDEX: return e->a->ty->k == TY_ARRAY ? root_mutable(e->a) : e->a->ty->k == TY_PTR;
    case E_FIELD:
        /* 'let' fields can only be assigned inside the constructor, through 'this' */
        if (e->fld && !e->fld->is_mut && !(cur_fn && cur_fn->is_ctor && e->a->k == E_VAR && !strcmp(e->a->name, "this"))) return 0;
        return e->a->ty->k == TY_PTR ? 1 : root_mutable(e->a);
    default: return 1;
    }
}
static void check_stmt(Stmt *s); static const char *opname(int op);

/* ---- strings: operators and methods become calls into the runtime prelude (lib/rt/prelude.svl) ---- */
static Func *rt_fn(const char *n) { for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, n)) return funcs[i]; die(0, "internal error: runtime function '%s' missing", n); return 0; }
static Expr *rt_call_into(Expr *c, const char *n, Expr *a, Expr *b, int line) {   /* call of a runtime function on checked arguments */
    Func *f = rt_fn(n);
    if (!c) c = mk(E_CALL, line);
    c->k = E_CALL; c->name = f->name; c->fn = f; c->a = c->b = 0; c->op = 0; c->builtin = 0; c->recv = 0; c->cast_ty = 0; c->lv = 0; c->tmp = c->dflag = 0;
    c->args = xalloc(sizeof(Expr *) * 2); c->args[0] = a; c->nargs = c->cargs = 1;
    if (b) { c->args[1] = b; c->nargs = c->cargs = 2; }
    c->ty = f->ret; if (has_drop(c->ty)) { c->tmp = alloc_slot(8, 8); register_temp(c); }
    return c;
}
static Expr *rt_call(const char *n, Expr *a, Expr *b, int line) { return rt_call_into(0, n, a, b, line); }
static Expr *mk_cast(Expr *x, Type *t) { if (same(x->ty, t)) return x; Expr *c = mk(E_CAST, x->line); c->a = x; c->cast_ty = t; c->ty = t; return c; }
static const char *str_conv(Expr **px) {   /* runtime function that turns *px into a String (0: it is one already) */
    Expr *x = *px; Type *t = x->ty;
    if (t->k == TY_NULL) { x->ty = &ty_str; return 0; }
    if (t->k == TY_STR) return 0;
    if (t->k == TY_ILIT) { retype(x, &ty_i64); return "rt_str_from_i64"; }
    if (t->k == TY_FLIT) { retype(x, &ty_f64); return "rt_str_from_f64"; }
    if (t->k == TY_BOOL) return "rt_str_from_bool";
    if (t->k == TY_INT && t->is_char) { *px = mk_cast(x, &ty_u8); return "rt_str_from_char"; }
    if (t->k == TY_INT && !t->sign) { *px = mk_cast(x, &ty_u64); return "rt_str_from_u64"; }
    if (t->k == TY_INT || t->k == TY_ENUM) { *px = mk_cast(x, &ty_i64); return "rt_str_from_i64"; }
    if (is_float(t)) { *px = mk_cast(x, &ty_f64); return "rt_str_from_f64"; }
    die(x->line, "cannot convert a value of type '%s' to a String", tname(t)); return 0;
}
static Expr *to_str(Expr *x) { const char *fn = str_conv(&x); return fn ? rt_call(fn, x, 0, x->line) : x; }
static Type *check_expr(Expr *e) {
    switch (e->k) {
    case E_INT: if (!e->ty) e->ty = &ty_ilit; return e->ty;
    case E_FLOAT: if (!e->ty) e->ty = &ty_flit; return e->ty;
    case E_BOOL: return e->ty = &ty_bool;
    case E_STR: return e->ty = &ty_str;
    case E_ENUMVAL: return e->ty;
    case E_SIZEOF: {
        Type *t = e->cast_ty; need_complete(t, e->line);
        if (t->k == TY_VOID) die(e->line, "sizeof(void)");
        e->k = E_INT; e->ival = t->size; return e->ty = &ty_u64;
    }
    case E_VAR: {
        Local *l = lookup(e->name);
        if (!l && !strcmp(e->name, "None")) {   /* None: an Option without a value, typed by its context */
            e->k = E_ERR;
            Type *rp = xalloc(sizeof *rp); rp->k = TY_RESPEND; rp->elem = &ty_void; rp->n = 1; rp->opt = 1; rp->size = rp->align = 8;
            return e->ty = rp;
        }
        if (!l) die(e->line, "undefined variable '%s'", e->name);
        if (l->is_global && has_ptr(l->ty) && !in_unsafe) die(e->line, "pointer global '%s' can only be used in unsafe code", l->name);
        if (l->moved && !assign_target) die(e->line, "use of moved value '%s'", e->name);
        if (l->is_const) { int ln = e->line; *e = *l->init; e->line = ln; return e->ty; }   /* a constant is its literal */
        e->lv = l; return e->ty = l->ty;
    }
    case E_SLIT: {   /* struct literal: every field exactly once */
        StructDef *sd = find_struct(e->name);
        char *set = xalloc((size_t)sd->nf + 1);
        for (int i = 0; i < e->nargs; i++) {
            Expr *fi = e->args[i]; int j = 0;
            while (j < sd->nf && strcmp(sd->f[j].name, fi->name)) j++;
            if (j == sd->nf) die(fi->line, "struct '%s' has no field '%s'", sd->name, fi->name);
            if (set[j]) die(fi->line, "field '%s' is given twice", fi->name);
            set[j] = 1; fi->fld = &sd->f[j]; fi->ty = fi->fld->ty;
            char ctx[200]; snprintf(ctx, sizeof ctx, "field '%s'", fi->name);
            check_expr(fi->a); expect_coerce(fi->a, fi->ty, ctx); consume(fi->a);
        }
        for (int j = 0; j < sd->nf; j++) if (!set[j]) die(e->line, "missing field '%s' in '%s' literal", sd->f[j].name, sd->name);
        e->ty = sd->ty; e->tmp = alloc_slot(sd->ty->size, sd->ty->align);
        if (has_drop(e->ty)) register_temp(e);
        return e->ty;
    }
    case E_FINIT: return e->ty;
    case E_REF: die(e->line, "'ref' is only allowed on call arguments"); return 0;
    case E_CALL: {
        if (!strcmp(e->name, "print") || !strcmp(e->name, "println")) {
            need_cap("Console", "printing", e->line);
            if (e->nargs != 1) die(e->line, "%s takes exactly one argument", e->name);
            Type *t = check_expr(e->args[0]); (void)t; finalize(e->args[0]); t = e->args[0]->ty;
            if (!(t->k == TY_STR || t->k == TY_BOOL || t->k == TY_INT || t->k == TY_FLOAT)) die(e->line, "cannot print a value of type '%s'", tname(t));
            e->builtin = e->name[5] ? 2 : 1;   /* 2 = println */
            return e->ty = &ty_void;
        }
        Func *f = 0; for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, e->name)) f = funcs[i];
        if (!f && !strcmp(e->name, "str")) {   /* str(x): text of a number, bool, char or String */
            if (e->nargs != 1) die(e->line, "str takes exactly one argument");
            check_expr(e->args[0]); finalize(e->args[0]);
            Expr *x = e->args[0]; const char *fn = str_conv(&x);
            if (fn) return rt_call_into(e, fn, x, 0, e->line)->ty;
            e->k = E_CAST; e->a = x; e->cast_ty = &ty_str; e->nargs = 0; return e->ty = &ty_str;   /* already a String */
        }
        if (!f && !strcmp(e->name, "take")) {   /* take(p): move the value out of *p without copying (unsafe, for containers) */
            if (!in_unsafe) die(e->line, "take() requires an unsafe block");
            if (e->nargs != 1) die(e->line, "take takes exactly one argument");
            Type *pt = check_expr(e->args[0]);
            if (pt->k != TY_PTR) die(e->line, "take needs a pointer, found '%s'", tname(pt));
            e->builtin = 7; return e->ty = pt->elem;
        }
        if (!f && !strcmp(e->name, "destroy")) {   /* destroy(p): run the destructor of *p (unsafe, for containers) */
            if (!in_unsafe) die(e->line, "destroy() requires an unsafe block");
            if (e->nargs != 1) die(e->line, "destroy takes exactly one argument");
            Type *pt = check_expr(e->args[0]);
            if (pt->k != TY_PTR) die(e->line, "destroy needs a pointer, found '%s'", tname(pt));
            e->builtin = 5; return e->ty = &ty_void;
        }
        if (!f && !strcmp(e->name, "yield_now")) {   /* let other tasks run */
            if (e->nargs != 0) die(e->line, "yield_now takes no arguments");
            uses_async = 1; e->builtin = 6; return e->ty = &ty_void;
        }
        if (!f && !strcmp(e->name, "super")) {   /* super(args): run the base class constructor on 'this' */
            if (!cur_fn || !cur_fn->is_ctor || !cur_fn->cls->base) die(e->line, "'super(...)' can only be called in the constructor of a class that extends another class");
            StructDef *b = cur_fn->cls->base; char cn[300]; snprintf(cn, sizeof cn, "%s.ctor", b->name);
            for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, cn)) f = funcs[i];
            Expr *th = mk(E_VAR, e->line); th->name = "this"; check_expr(th);
            Expr *r = mk(E_REF, e->line); r->a = th; r->ty = b->ty;
            e->args = realloc(e->args, (size_t)(e->nargs + 1) * sizeof *e->args);
            memmove(e->args + 1, e->args, (size_t)e->nargs * sizeof *e->args);
            e->args[0] = r; e->nargs++; e->cargs = e->nargs; e->recv = 1; e->name = f->name;
        }
        int ctor = 0;
        if (!f) {   /* Person(args) constructs a class value */
            StructDef *csd = find_struct(e->name);
            if (csd && csd->is_class) {
                char cn[300]; snprintf(cn, sizeof cn, "%s.ctor", csd->name);
                for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, cn)) f = funcs[i];
                ctor = 1;
            }
        }
        if (!f && (!strcmp(e->name, "Ok") || !strcmp(e->name, "Err"))) {
            int er = e->name[0] == 'E';
            if (er ? e->nargs != 1 : e->nargs > 1) die(e->line, "'%s' takes %s argument", e->name, er ? "exactly one" : "at most one");
            Type *pt = &ty_void;
            if (e->nargs == 1) { e->a = e->args[0]; pt = check_expr(e->a); }
            e->k = er ? E_ERR : E_OK;
            Type *rp = xalloc(sizeof *rp); rp->k = TY_RESPEND; rp->elem = pt; rp->n = er; rp->size = rp->align = 8;
            return e->ty = rp;
        }
        if (!f && !strcmp(e->name, "Some")) {   /* Some(x): an Option holding x, typed by its context */
            if (e->nargs != 1) die(e->line, "'Some' takes exactly one argument");
            e->a = e->args[0]; Type *pt = check_expr(e->a);
            if (pt->k == TY_VOID) die(e->line, "'Some' needs a value");
            e->k = E_OK;
            Type *rp = xalloc(sizeof *rp); rp->k = TY_RESPEND; rp->elem = pt; rp->opt = 1; rp->size = rp->align = 8;
            return e->ty = rp;
        }
        int pre = 0;
        if (!f) { Generic *gn = find_generic(e->name); if (gn && gn->kind == 2) { f = infer_generic(gn, e); e->name = f->name; pre = 1; } }
        if (!f) die(e->line, "call to undefined function '%s'", e->name);
        if (f->is_unsafe && !in_unsafe) die(e->line, "call to %s function '%s' requires an unsafe block", f->is_extern ? "extern" : "unsafe", f->name);
        if (f->is_deprecated && !file_trusted[POS_FILE(e->line)] && !(cur_fn && cur_fn->is_deprecated)) warn(e->line, "'%s' is deprecated", f->name);
        for (int i = 0; i < f->nreq; i++) { char what[300]; snprintf(what, sizeof what, "calling '%s'", f->name); need_cap(f->req[i], what, e->line); }
        if (f->is_extern) {
            char what[200]; int known; const char *c = extern_cap(f->name, &known);
            if (f->is_unsafe) need_cap("Native", "calling an extern function", e->line);
            snprintf(what, sizeof what, known ? "calling '%s'" : "calling unclassified extern function '%s'", f->name);
            if (c) need_cap(c, what, e->line);
            if (!strcmp(f->name, "fopen")) {   /* write modes also need File.Write; unknown mode counts as write */
                Expr *m = e->nargs > 1 ? e->args[1] : 0; int w = 1;
                if (m && m->k == E_STR) { w = 0; for (int i = 0; i < m->slen; i++) if (strchr("wa+", m->sval[i])) w = 1; }
                if (w) need_cap("File.Write", "calling 'fopen' with a write mode (or a mode that is not a literal)", e->line);
            }
        }
        if (e->nargs + ctor != f->nparams) die(e->line, "'%s' expects %d argument(s), got %d", ctor ? e->name : f->name, f->nparams - (ctor || e->recv), e->nargs - e->recv);
        for (int i = 0; i < e->nargs; i++) {
            Expr *a = e->args[i]; int pi = i + ctor, an = i + 1 - e->recv;
            if (e->recv && i == 0) continue;   /* method receiver: already checked */
            if (f->pref[pi]) {
                if (a->k != E_REF) die(a->line, "argument %d of '%s' must be passed as 'ref'", an, f->name);
                Type *t = pre ? a->a->ty : check_expr(a->a);
                if (!is_lvalue(a->a)) die(a->line, "'ref' needs an assignable place");
                if (!root_mutable(a->a)) die(a->line, "cannot pass immutable value by 'ref'; declare it with 'var'");
                if (!same(t, f->pty[pi]) && !(t->k == TY_STRUCT && f->pty[pi]->k == TY_STRUCT && is_sub(t->sd, f->pty[pi]->sd)))
                    die(a->line, "argument %d of '%s': expected '%s', found '%s'", an, f->name, tname(f->pty[pi]), tname(t));
                a->ty = t;
            } else {
                if (a->k == E_REF) die(a->line, "parameter %d of '%s' is not a 'ref' parameter", an, f->name);
                if (!pre) check_expr(a);
                expect_coerce(a, f->pty[pi], "argument");
                if (!f->is_extern && !f->borrow) consume(a);   /* extern and runtime functions only borrow */
                if (f->is_extern && f->pty[pi]->k == TY_STR && a->k != E_STR) {   /* C gets "" for an empty (null) String */
                    Expr *w = mk(E_CALL, a->line); w->name = "cstr"; w->builtin = 9; w->ty = &ty_str;
                    w->args = xalloc(sizeof(Expr *)); w->args[0] = a; w->nargs = w->cargs = 1; e->args[i] = w;
                }
            }
        }
        e->fn = f;
        if (ctor) { e->builtin = 3; e->tmp = alloc_slot(f->cls->ty->size, 8); e->ty = f->cls->ty; if (has_drop(e->ty)) register_temp(e); return e->ty; }
        if (f->is_async) { uses_async = 1; e->ty = mk_task(f->ret); e->tmp = alloc_slot(8, 8); register_temp(e); return e->ty; }
        if (f->is_extern && f->ret->k == TY_STR) die(e->line, "extern function '%s' cannot return String; declare it to return *u8 and convert with 'as String'", f->name);
        if (is_agg(f->ret)) e->tmp = alloc_slot(f->ret->size, f->ret->align);
        else if (has_drop(f->ret)) e->tmp = alloc_slot(8, 8);
        e->ty = f->ret; if (has_drop(e->ty)) register_temp(e);
        return e->ty;
    }
    case E_UN: {
        Type *t = check_expr(e->a);
        switch (e->op) {
        case O_NEG: if (!(t->k == TY_ILIT || t->k == TY_FLIT || is_float(t) || (is_int(t) && t->sign))) die(e->line, "cannot negate '%s'", tname(t)); return e->ty = t;
        case O_NOT: if (t->k != TY_BOOL) die(e->line, "'!' needs bool, found '%s'", tname(t)); return e->ty = &ty_bool;
        case O_BNOT: if (!is_int(t) && t->k != TY_ILIT) die(e->line, "'~' needs an integer"); return e->ty = t;
        case O_DEREF: if (!in_unsafe) die(e->line, "pointer dereference requires an unsafe block");
            if (t->k != TY_PTR) die(e->line, "cannot dereference '%s'", tname(t)); return e->ty = t->elem;
        case O_ADDR: if (!in_unsafe) die(e->line, "taking an address requires an unsafe block");
            if (!is_lvalue(e->a)) die(e->line, "'&' needs an assignable place"); return e->ty = mk_ptr(t);
        }
        return 0;
    }
    case E_CAST: {
        Type *from = check_expr(e->a); (void)from; finalize(e->a); from = e->a->ty; Type *to = e->cast_ty;
        int fi = is_int(from) || from->k == TY_BOOL || from->k == TY_ENUM, ti = is_int(to);
        if (fi && ti) { }
        else if ((is_int(from) || is_float(from)) && (is_int(to) || is_float(to))) { }   /* numeric conversions */
        else if ((from->k == TY_PTR && (ti || to->k == TY_PTR || to->k == TY_STR)) || (is_int(from) && to->k == TY_PTR) || (from->k == TY_STR && (to->k == TY_PTR || ti))) { if (!in_unsafe) die(e->line, "pointer casts require an unsafe block"); }
        else if (from->k == TY_STR && to->k == TY_STR) { }
        else if (to->k == TY_STR && (fi || is_float(from))) die(e->line, "cannot convert '%s' to String with 'as'; use str(x) or \"\" + x", tname(from));
        else die(e->line, "invalid cast from '%s' to '%s'", tname(from), tname(to));
        if (to->k == TY_STR && from->k != TY_STR) {
            if (from->k != TY_PTR) die(e->line, "only a pointer can be converted to String with 'as'; use str() for numbers");
            if (rt_fidx < 0 || (e->line >> 26) != rt_fidx) return rt_call_into(e, "rt_str_from_c", e->a, 0, e->line)->ty;   /* copies the C string */
        }
        return e->ty = to;
    }
    case E_INDEX: {
        Type *bt = check_expr(e->a), *it = check_expr(e->b);
        if (it->k == TY_ILIT) retype(e->b, &ty_u64); else if (!is_int(it)) die(e->line, "index must be an integer");
        if (bt->k == TY_ARRAY) return e->ty = bt->elem;
        if (bt->k == TY_PTR) { if (!in_unsafe) die(e->line, "pointer indexing requires an unsafe block"); return e->ty = bt->elem; }
        if (bt->k == TY_STR) return e->ty = &ty_u8;   /* bounds-checked byte */
        die(e->line, "cannot index '%s'", tname(bt)); return 0;
    }
    case E_FIELD: {
        if (e->a->k == E_VAR && !lookup(e->a->name)) {
            EnumDef *ed = find_enum(e->a->name);
            if (ed) {
                for (int i = 0; i < ed->nv; i++) if (!strcmp(ed->vn[i], e->name)) {
                    if (!ed->data) { e->k = E_ENUMVAL; e->ival = i; return e->ty = ed->ty; }
                    if (ed->vi[i].nf) { char qn[300]; snprintf(qn, sizeof qn, "%s.%s", ed->name, e->name); die(e->line, "'%s' carries %d value(s): write %s(...)", qn, ed->vi[i].nf, qn); }
                    e->k = E_VARIANT; e->ival = i; e->nargs = 0; e->a = 0; e->ty = ed->ty; e->tmp = alloc_slot(ed->ty->size, 8);
                    if (has_drop(e->ty)) register_temp(e);
                    return e->ty;
                }
                die(e->line, "enum '%s' has no variant '%s'", ed->name, e->name);
            }
        }
        Type *bt = check_expr(e->a);
        if (bt->k == TY_PTR) { if (!in_unsafe) die(e->line, "field access through a pointer requires an unsafe block"); bt = bt->elem; }
        if (bt->k != TY_STRUCT) die(e->line, "'%s' has no fields", tname(bt));
        for (int i = 0; i < bt->sd->nf; i++) if (!strcmp(bt->sd->f[i].name, e->name)) {
            e->fld = &bt->sd->f[i];
            if (!can_access(e->fld->vis, e->fld->owner)) die(e->line, "'%s' is %s in class '%s'", e->name, vis_name(e->fld->vis), e->fld->owner->name);
            return e->ty = e->fld->ty;
        }
        die(e->line, "struct '%s' has no field '%s'", bt->sd->name, e->name); return 0;
    }
    case E_MCALL: {
        Expr *o = e->a; StructDef *sd = 0; int stat = 0; Expr *recv = 0; Iface *ifc = 0;
        if (o->k == E_VAR && !strcmp(o->name, "super") && !lookup("super")) {   /* super.method(): the base class version */
            if (!cur_fn || !cur_fn->cls || !cur_fn->cls->base || !lookup("this")) die(e->line, "'super' is only available in methods of a class that extends another class");
            sd = cur_fn->cls->base;
            Expr *th = mk(E_VAR, e->line); th->name = "this"; check_expr(th);
            recv = mk(E_REF, e->line); recv->a = th; recv->ty = sd->ty;
        } else if (o->k == E_VAR && !lookup(o->name) && find_enum(o->name)) {   /* Enum.Variant(values) */
            EnumDef *ed = find_enum(o->name); int vi = -1;
            for (int i = 0; i < ed->nv; i++) if (!strcmp(ed->vn[i], e->name)) vi = i;
            if (vi < 0) die(e->line, "enum '%s' has no variant '%s'", ed->name, e->name);
            if (!ed->data || !ed->vi[vi].nf) die(e->line, "'%s.%s' carries no values: write it without parentheses", ed->name, e->name);
            VarInfo *v = &ed->vi[vi];
            char qn[300]; snprintf(qn, sizeof qn, "%s.%s", ed->name, e->name);
            if (e->nargs != v->nf) die(e->line, "'%s' takes %d value(s), found %d", qn, v->nf, e->nargs);
            for (int i = 0; i < e->nargs; i++) {
                char ctx[400]; snprintf(ctx, sizeof ctx, "value %d of '%s'", i + 1, qn);
                check_expr(e->args[i]); expect_coerce(e->args[i], v->ft[i], ctx); consume(e->args[i]);
            }
            e->k = E_VARIANT; e->ival = vi; e->a = 0; e->ty = ed->ty; e->tmp = alloc_slot(ed->ty->size, 8);
            if (has_drop(e->ty)) register_temp(e);
            return e->ty;
        } else if (o->k == E_VAR && !lookup(o->name)) {
            sd = find_struct(o->name);
            if (!sd || !sd->is_class) die(o->line, "undefined variable '%s'", o->name);
            stat = 1;
        } else {
            Type *ot = check_expr(o), *ct = ot;
            if (ct->k == TY_IFACE) { ifc = ct->ifc; recv = o; }
            else if (ct->k == TY_STR) {   /* s.len() is built in; s.name(args) calls the runtime's str_name(s, args) */
                e->args = realloc(e->args, (size_t)(e->nargs + 1) * sizeof *e->args);
                memmove(e->args + 1, e->args, (size_t)e->nargs * sizeof *e->args);
                e->args[0] = o; e->nargs++; e->cargs = e->nargs; e->a = 0;
                if (!strcmp(e->name, "len")) {
                    if (e->nargs != 1) die(e->line, "len takes no arguments");
                    e->k = E_CALL; e->builtin = 8; return e->ty = &ty_i64;
                }
                char fnm[300]; snprintf(fnm, sizeof fnm, "str_%s", e->name); Func *sf = 0;
                for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, fnm)) sf = funcs[i];
                if (!sf || !sf->borrow || sf->nparams < 1 || sf->pty[0]->k != TY_STR) die(e->line, "'String' has no method '%s'", e->name);
                e->k = E_CALL; e->name = sf->name; e->recv = 1; return check_expr(e);
            }
            else {
                if (ct->k == TY_PTR) {
                    if (!in_unsafe) die(e->line, "method call through a pointer requires an unsafe block");
                    ct = ct->elem;
                    Expr *d = mk(E_UN, e->line); d->op = O_DEREF; d->a = o; d->ty = ct; o = d;
                }
                if (ct->k != TY_STRUCT || !ct->sd->is_class) die(e->line, "'%s' has no methods", tname(ot));
                sd = ct->sd;
                recv = mk(E_REF, e->line); recv->a = o; recv->ty = ct;
            }
        }
        Func *f = 0; char full[300];
        if (ifc) {
            snprintf(full, sizeof full, "%s.%s", ifc->name, e->name);
            for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, full) && funcs[i]->ifc == ifc) f = funcs[i];
            if (!f) die(e->line, "interface '%s' has no method '%s'", ifc->name, e->name);
        } else {
            for (StructDef *c = sd; c && !f; c = c->base) {
                snprintf(full, sizeof full, "%s.%s", c->name, e->name);
                for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, full) && !funcs[i]->is_ctor && !funcs[i]->is_dtor) f = funcs[i];
            }
            if (!f) die(e->line, "class '%s' has no method '%s'", sd->name, e->name);
            if (stat && !f->is_static) die(e->line, "'%s' is an instance method; call it on an object", full);
            if (!stat && f->is_static) die(e->line, "'%s' is a static method; call it as %s(...)", full, full);
            if (!can_access(f->vis, f->cls)) die(e->line, "'%s' is %s in class '%s'", e->name, vis_name(f->vis), f->cls->name);
        }
        if (recv) {
            e->args = realloc(e->args, (size_t)(e->nargs + 1) * sizeof *e->args);
            memmove(e->args + 1, e->args, (size_t)e->nargs * sizeof *e->args);
            e->args[0] = recv; e->nargs++; e->cargs = e->nargs; e->recv = 1;
        }
        e->k = E_CALL; e->name = xstrn(full, strlen(full));
        return check_expr(e);
    }
    case E_TOIFACE: return e->ty;
    case E_OK: case E_ERR: case E_VARIANT: return e->ty;
    case E_AWAIT: {   /* wait for a task and take its result */
        Type *t = check_expr(e->a);
        if (t->k != TY_TASK) die(e->line, "'await' needs a Task (the result of calling an async fn), found '%s'", tname(t));
        consume(e->a); uses_async = 1;
        e->ty = t->elem; e->tmp = alloc_slot(t->elem->size > 8 ? t->elem->size : 8, 8);
        if (has_drop(e->ty)) register_temp(e);
        return e->ty;
    }
    case E_TRY: {
        Type *t = check_expr(e->a);
        if (t->k != TY_RESULT) die(e->line, "'?' needs a Result or an Option, found '%s'", tname(t));
        if (t->opt && (!cur_fn || cur_fn->ret->k != TY_RESULT || !cur_fn->ret->opt)) die(e->line, "'?' on an Option can only be used in a function that returns an Option");
        if (!t->opt && (!cur_fn || cur_fn->ret->k != TY_RESULT || cur_fn->ret->opt)) die(e->line, "'?' can only be used in a function that returns a Result");
        if (!same(t->err, cur_fn->ret->err)) die(e->line, "'?' cannot convert error type '%s' into '%s'", tname(t->err), tname(cur_fn->ret->err));
        consume(e->a);
        e->ty = t->elem; if (has_drop(e->ty)) register_temp(e);
        return e->ty;
    }
    case E_BIN: {
        int op = e->op; Type *lt = check_expr(e->a), *rt = check_expr(e->b);
        if (op == O_LAND || op == O_LOR) { if (lt->k != TY_BOOL || rt->k != TY_BOOL) die(e->line, "'&&' / '||' need bool operands"); return e->ty = &ty_bool; }
        if (op == O_ADD && (lt->k == TY_STR || rt->k == TY_STR)) {   /* concatenation; the other side is converted */
            Expr *a = to_str(e->a), *b = to_str(e->b);
            return rt_call_into(e, "rt_str_concat", a, b, e->line)->ty;
        }
        if (lt->k == TY_PTR && (op == O_ADD || op == O_SUB)) {
            if (!in_unsafe) die(e->line, "pointer arithmetic requires an unsafe block");
            if (rt->k == TY_ILIT) retype(e->b, &ty_u64); else if (!is_int(rt)) die(e->line, "pointer offset must be an integer");
            return e->ty = lt;
        }
        if (op == O_SHL || op == O_SHR) {
            if (!(is_int(lt) || lt->k == TY_ILIT) || !(is_int(rt) || rt->k == TY_ILIT)) die(e->line, "shifts need integer operands");
            if (lt->k == TY_ILIT && is_int(rt)) retype(e->a, rt); else if (rt->k == TY_ILIT && is_int(lt)) retype(e->b, lt);
            return e->ty = e->a->ty;
        }
        if (lt->k == TY_NULL && (rt->k == TY_PTR || rt->k == TY_STR)) e->a->ty = rt; else if (rt->k == TY_NULL && (lt->k == TY_PTR || lt->k == TY_STR)) e->b->ty = lt;
        unify(e->a, e->b); lt = e->a->ty; rt = e->b->ty;
        if (!same(lt, rt)) die(e->line, "mismatched operand types '%s' and '%s'", tname(lt), tname(rt));
        if ((op == O_DIV || op == O_MOD) && e->b->k == E_INT && e->b->ival == 0 && !is_float(lt) && lt->k != TY_FLIT) die(e->line, "division by zero");
        if (op >= O_EQ && op <= O_GE && lt->k == TY_STR) {   /* compare the contents: rt_str_cmp(a, b) <op> 0 */
            Expr *c = rt_call("rt_str_cmp", e->a, e->b, e->line), *z = mk(E_INT, e->line); z->ty = &ty_i32;
            e->a = c; e->b = z; return e->ty = &ty_bool;
        }
        if (op >= O_EQ && op <= O_GE) {
            if (lt->k == TY_ILIT) { retype(e->a, &ty_i32); retype(e->b, &ty_i32); lt = &ty_i32; }
            if (lt->k == TY_FLIT) { retype(e->a, &ty_f64); retype(e->b, &ty_f64); lt = &ty_f64; }
            if (is_float(lt)) return e->ty = &ty_bool;
            int eq = op == O_EQ || op == O_NE;
            if (!(is_int(lt) || lt->k == TY_PTR || (eq && (lt->k == TY_BOOL || lt->k == TY_ENUM || lt->k == TY_STR)))) die(e->line, "cannot compare '%s' values with this operator", tname(lt));
            return e->ty = &ty_bool;
        }
        if (is_float(lt) || lt->k == TY_FLIT) {
            if (op > O_DIV) die(e->line, "'%s' is not supported for floating point", opname(op));
            return e->ty = lt;
        }
        if (!(is_int(lt) || lt->k == TY_ILIT)) die(e->line, "arithmetic needs integer operands, found '%s'", tname(lt));
        return e->ty = lt;
    }
    }
    return 0;
}

static int always_ret(Stmt *s) {
    switch (s->k) {
    case S_RET: return 1;
    case S_BLOCK: for (int i = 0; i < s->n; i++) if (always_ret(s->list[i])) return 1; return 0;
    case S_IF: return s->els && always_ret(s->body) && always_ret(s->els);
    case S_MATCH: for (int i = 0; i < s->n; i++) if (!always_ret(s->list[i])) return 0; return 1;
    default: return 0;
    }
}
static void check_scope_body(Stmt *b) {
    int mark = nlocals, off = cur_off, base = scope_base; scope_base = nlocals;
    if (b->k == S_BLOCK) { int u = b->unsafe_blk; in_unsafe += u; for (int i = 0; i < b->n; i++) check_stmt(b->list[i]); in_unsafe -= u; }
    else check_stmt(b);
    nlocals = mark; cur_off = off; scope_base = base;
}
static void check_cond(Expr *c) { Type *t = check_expr(c); if (t->k != TY_BOOL) die(c->line, "condition must be bool, found '%s'", tname(t)); }

static void check_stmt0(Stmt *s);
/* for x in c { body }  with c a collection class that has len() and item(i64):
 *   for __idx in 0..c.len() { let x = c.item(__idx); body }   (the length is read once; item() checks bounds) */
static Expr *clone_raw(Expr *e) {
    if (!e) return 0;
    Expr *c = mk(e->k, e->line); *c = *e;
    c->a = clone_raw(e->a); c->b = clone_raw(e->b);
    if (e->nargs) { c->args = xalloc(sizeof(Expr *) * (size_t)e->nargs); for (int i = 0; i < e->nargs; i++) c->args[i] = clone_raw(e->args[i]); }
    return c;
}
static int simple_place(Expr *e) { if (e->k == E_STR) return 1; while (e->k == E_FIELD) e = e->a; return e->k == E_VAR; }
static Type *check_expr(Expr *e);
static void desugar_foreach(Stmt *s) {
    if (!simple_place(s->e)) return;
    Type *t = check_expr(clone_raw(s->e));
    if (t->k == TY_STR || (t->k == TY_ARRAY && (is_agg(t->elem) || has_drop(t->elem)))) {   /* for c in s / for x in arr: by index */
        if (t->k == TY_ARRAY && move_only(t->elem)) die(s->line, "cannot iterate by value over elements of type '%s' (it has a destructor); loop over the indices instead", tname(t->elem));
        int line = s->line;
        Expr *zero = mk(E_INT, line); zero->ty = &ty_ilit;
        Expr *n;
        if (t->k == TY_STR) { n = mk(E_MCALL, line); n->a = clone_raw(s->e); n->name = "len"; }
        else { n = mk(E_INT, line); n->ival = t->n; n->ty = &ty_i64; }
        Expr *iv = mk(E_VAR, line); iv->name = "__idx";
        Expr *item = mk(E_INDEX, line); item->a = clone_raw(s->e); item->b = iv;
        Stmt *let = mks(S_LET, line); let->name = s->name; let->e = item;
        Stmt *blk = mks(S_BLOCK, line); VPUSH(blk->list, blk->n, blk->cap, let); VPUSH(blk->list, blk->n, blk->cap, s->body);
        s->is_range = 1; s->name = "__idx"; s->e = zero; s->e2 = n; s->body = blk;
        return;
    }
    if (t->k != TY_STRUCT || !t->sd->is_class) return;
    char full[300]; int has_len = 0, has_item = 0; Type *et = 0;
    for (StructDef *c = t->sd; c; c = c->base) for (int i = 0; i < nfuncs; i++) {
        snprintf(full, sizeof full, "%s.len", c->name); if (!strcmp(funcs[i]->name, full)) has_len = 1;
        snprintf(full, sizeof full, "%s.item", c->name); if (!strcmp(funcs[i]->name, full) && !has_item) { has_item = 1; et = funcs[i]->ret; }
    }
    if (!has_len || !has_item) die(s->line, "cannot iterate over '%s': it needs methods len() and item(i: i64)", tname(t));
    if (move_only(et)) die(s->line, "cannot iterate by value over elements of type '%s' (it has a destructor); loop over the indices instead", tname(et));
    int line = s->line;
    Expr *zero = mk(E_INT, line); zero->ty = &ty_ilit;
    Expr *len = mk(E_MCALL, line); len->a = clone_raw(s->e); len->name = "len";
    Expr *iv = mk(E_VAR, line); iv->name = "__idx";
    Expr *item = mk(E_MCALL, line); item->a = clone_raw(s->e); item->name = "item";
    item->args = xalloc(sizeof(Expr *)); item->args[0] = iv; item->nargs = item->cargs = 1;
    Stmt *let = mks(S_LET, line); let->name = s->name; let->e = item;
    Stmt *blk = mks(S_BLOCK, line); VPUSH(blk->list, blk->n, blk->cap, let); VPUSH(blk->list, blk->n, blk->cap, s->body);
    s->is_range = 1; s->name = "__idx"; s->e = zero; s->e2 = len; s->body = blk;
}
static void check_stmt(Stmt *s) { Stmt *prev = cur_stmt; cur_stmt = s; check_stmt0(s); cur_stmt = s; close_temps(s); cur_stmt = prev; }
static void check_stmt0(Stmt *s) {
    switch (s->k) {
    case S_BLOCK: check_scope_body(s); break;
    case S_LET: {
        Type *t = s->dty;
        if (s->e) {
            check_expr(s->e);
            if (t) expect_coerce(s->e, t, "initializer"); else { finalize(s->e); t = s->e->ty; }
        }
        if (t->k == TY_VOID) die(s->line, "cannot declare a variable of type void");
        if (t->k == TY_NULL) die(s->line, "cannot infer a type from 'null'; add a type annotation");
        if (has_iface(t)) die(s->line, IFACE_MSG, tname(t));
        need_complete(t, s->line); require_safe_type(t, s->line); s->dty = t;
        if (s->e) consume(s->e);
        s->lv = declare(s->name, t, s->is_mut, 0, s->line); break;
    }
    case S_ASSIGN: {
        if (s->e->k == E_VAR) { Local *cl = lookup(s->e->name); if (cl && cl->is_const) die(s->line, "cannot assign to constant '%s'", cl->name); }
        assign_target = s->e->k == E_VAR; Type *lt = check_expr(s->e); assign_target = 0;
        if (s->e->k == E_INDEX && s->e->a->ty->k == TY_STR) die(s->line, "the bytes of a String cannot be changed (strings are immutable); build a new String instead");
        if (!is_lvalue(s->e)) die(s->line, "left side of assignment is not assignable");
        if (!root_mutable(s->e)) die(s->line, "cannot assign to immutable value; declare it with 'var'");
        check_expr(s->e2); expect_coerce(s->e2, lt, "assignment"); consume(s->e2);
        if (s->e->k == E_VAR) s->e->lv->moved = 0;   /* assigning revives a moved variable */
        if (cur_fn->is_ctor && s->e->k == E_FIELD && s->e->a->k == E_VAR && !strcmp(s->e->a->name, "this") && has_drop(lt) && !s->e->fld->init) {
            int top = 0, fi = (int)(s->e->fld - cur_fn->cls->f);
            for (int i = 0; i < cur_fn->body->n; i++) if (cur_fn->body->list[i] == s) top = 1;
            if (top && fi < 256 && !ctor_set[fi]) s->init_store = 1;
            if (fi < 256) ctor_set[fi] = 1;
        }
        break;
    }
    case S_EXPR: check_expr(s->e); finalize(s->e); break;
    case S_IF: {
        check_cond(s->e); close_temps(s);
        int n0 = nlocals; char *pre = snap_moved();
        check_scope_body(s->body); char *a1 = snap_moved(); int r1 = always_ret(s->body);
        set_moved(pre, n0); if (s->els) check_scope_body(s->els); char *a2 = snap_moved(); int r2 = s->els && always_ret(s->els);
        for (int i = 0; i < n0; i++) locals[i]->moved = (r1 && !r2) ? a2[i] : (r2 && !r1) ? a1[i] : (a1[i] | a2[i]);   /* moved on any path that continues */
        break;
    }
    case S_WHILE: check_cond(s->e); close_temps(s); loop_depth++; check_scope_body(s->body); loop_depth--; break;
    case S_FOR: {
        if (!s->is_range) desugar_foreach(s);
        int mark = nlocals, off = cur_off, base = scope_base; scope_base = nlocals;
        Type *vt;
        if (s->is_range) {
            check_expr(s->e); check_expr(s->e2);
            unify(s->e, s->e2);
            if (s->e->ty->k == TY_ILIT) { retype(s->e, &ty_i32); retype(s->e2, &ty_i32); }
            if (!is_int(s->e->ty) || !same(s->e->ty, s->e2->ty)) die(s->line, "range bounds must be integers of the same type");
            vt = s->e->ty; s->slot1 = alloc_slot(8, 8);
        } else {
            Type *at = check_expr(s->e);
            if (at->k != TY_ARRAY) die(s->line, "cannot iterate over a value of type '%s' (use a range a..b, an array, a String or a class with len() and item(i: i64))", tname(at));
            if (is_agg(at->elem)) die(s->line, "to iterate over elements of type '%s', store the array in a variable first", tname(at->elem));
            vt = at->elem; s->slot1 = alloc_slot(8, 8);
        }
        s->lv = declare(s->name, vt, 0, 0, s->line);
        loop_depth++; check_scope_body(s->body); loop_depth--;
        nlocals = mark; cur_off = off; scope_base = base; break;
    }
    case S_RET:
        if (s->e) { if (cur_fn->ret->k == TY_VOID) die(s->line, "void function cannot return a value"); check_expr(s->e); expect_coerce(s->e, cur_fn->ret, "return"); consume(s->e); }
        else if (cur_fn->ret->k != TY_VOID) die(s->line, "missing return value");
        break;
    case S_BRK: case S_CONT: if (!loop_depth) die(s->line, "'%s' outside of a loop", s->k == S_BRK ? "break" : "continue"); break;
    case S_MATCH: {
        Type *t = check_expr(s->e); finalize(s->e); t = s->e->ty;
        consume(s->e); close_temps(s);
        int n0 = nlocals; char *pre = snap_moved(), *acc = xalloc((size_t)n0 + 1); int anyc = 0;
#define ARM_BEGIN() set_moved(pre, n0)
#define ARM_END(b) do { if (!always_ret(b)) { anyc = 1; for (int q = 0; q < n0; q++) acc[q] |= (char)locals[q]->moved; } } while (0)
        if (t->k == TY_DATA) {   /* Enum.Variant(a, b) binds the values, Enum.Variant matches a variant without values */
            EnumDef *ed = t->ed;
            s->slot1 = alloc_slot(8, 8); s->vlv = xalloc(sizeof(Local **) * (size_t)(s->n + 1));
            int wild = 0, ncov = 0; char *cov = xalloc((size_t)ed->nv + 1);
            for (int i = 0; i < s->n; i++) {
                Expr *p = s->pats[i];
                if (!p) { wild = 1; ARM_BEGIN(); check_scope_body(s->list[i]); ARM_END(s->list[i]); continue; }
                if (!((p->k == E_MCALL || p->k == E_FIELD) && p->a->k == E_VAR))
                    die(p->line, "match patterns on '%s' must be %s.Variant(names) or %s.Variant", ed->name, ed->name, ed->name);
                if (strcmp(p->a->name, ed->name)) die(p->line, "pattern '%s.%s' does not belong to enum '%s'", p->a->name, p->name, ed->name);
                int vi = -1;
                for (int j = 0; j < ed->nv; j++) if (!strcmp(ed->vn[j], p->name)) vi = j;
                if (vi < 0) die(p->line, "enum '%s' has no variant '%s'", ed->name, p->name);
                VarInfo *v = &ed->vi[vi]; int nb = p->k == E_MCALL ? p->nargs : 0;
                if (nb != v->nf) {
                    if (!v->nf) die(p->line, "pattern '%s.%s' takes no bindings", ed->name, p->name);
                    char qn[300]; snprintf(qn, sizeof qn, "%s.%s", ed->name, p->name);
                    die(p->line, "pattern '%s' needs %d binding(s), found %d", qn, v->nf, nb);
                }
                for (int j = 0; j < nb; j++) if (p->args[j]->k != E_VAR) die(p->args[j]->line, "a pattern binding must be a name or '_'");
                p->ival = vi; if (!cov[vi]) { cov[vi] = 1; ncov++; }
                ARM_BEGIN();
                int mark = nlocals, off = cur_off, base = scope_base; scope_base = nlocals;
                s->vlv[i] = xalloc(sizeof(Local *) * MAXVF);
                for (int j = 0; j < nb; j++) if (strcmp(p->args[j]->name, "_")) s->vlv[i][j] = declare(p->args[j]->name, v->ft[j], 0, 0, p->line);
                check_scope_body(s->list[i]);
                nlocals = mark; cur_off = off; scope_base = base;
                ARM_END(s->list[i]);
            }
            if (!wild && ncov < ed->nv) die(s->line, "non-exhaustive match: not all variants of '%s' are covered (add '_ =>')", ed->name);
            set_moved(anyc ? acc : pre, n0);
            break;
        }
        if (t->k == TY_RESULT) {
            s->slot1 = alloc_slot(8, 8); s->alv = xalloc(sizeof(Local *) * (size_t)(s->n + 1));
            int wild = 0, cov[2] = { 0, 0 };
            for (int i = 0; i < s->n; i++) {
                Expr *p = s->pats[i];
                if (!p) wild = 1;
                else {
                    if (t->opt) {
                        if (!((p->k == E_CALL && !strcmp(p->name, "Some") && p->nargs == 1 && p->args[0]->k == E_VAR) || (p->k == E_VAR && !strcmp(p->name, "None"))))
                            die(p->line, "match patterns on an Option must be Some(name) or None");
                    } else if (!(p->k == E_CALL && (!strcmp(p->name, "Ok") || !strcmp(p->name, "Err")) && p->nargs <= 1 && (p->nargs == 0 || p->args[0]->k == E_VAR)))
                        die(p->line, "match patterns on a Result must be Ok(name) or Err(name)");
                    int er = p->name[0] == 'E' || p->name[0] == 'N'; Type *pt = er ? t->err : t->elem;
                    if (pt->k == TY_VOID ? p->nargs != 0 : p->nargs != 1) die(p->line, "pattern '%s' needs %s", p->name, pt->k == TY_VOID ? "no binding" : "exactly one binding");
                    cov[er] = 1;
                    ARM_BEGIN();
                    int mark = nlocals, off = cur_off, base = scope_base; scope_base = nlocals;
                    if (p->nargs == 1 && strcmp(p->args[0]->name, "_")) s->alv[i] = declare(p->args[0]->name, pt, 0, 0, p->line);
                    check_scope_body(s->list[i]);
                    nlocals = mark; cur_off = off; scope_base = base;
                    ARM_END(s->list[i]);
                    continue;
                }
                ARM_BEGIN(); check_scope_body(s->list[i]); ARM_END(s->list[i]);
            }
            if (!wild && !(cov[0] && cov[1])) die(s->line, t->opt ? "non-exhaustive match on Option: handle both Some and None (or add '_ =>')" : "non-exhaustive match on Result: handle both Ok and Err (or add '_ =>')");
            set_moved(anyc ? acc : pre, n0);
            break;
        }
        if (!(is_int(t) || t->k == TY_BOOL || t->k == TY_ENUM)) die(s->line, "cannot match on '%s'", tname(t));
        s->slot1 = alloc_slot(8, 8);
        int wild = 0, ncov = 0; char *cov = xalloc(t->k == TY_ENUM ? t->ed->nv + 1 : 3);
        for (int i = 0; i < s->n; i++) {
            Expr *p = s->pats[i];
            if (!p) wild = 1;
            else {
                check_expr(p);
                if (!(p->k == E_INT || p->k == E_BOOL || p->k == E_ENUMVAL)) die(p->line, "match patterns must be constants");
                expect_coerce(p, t, "pattern");
                if (t->k == TY_ENUM || t->k == TY_BOOL) { int idx = (int)p->ival; if (!cov[idx]) { cov[idx] = 1; ncov++; } }
            }
            ARM_BEGIN(); check_scope_body(s->list[i]); ARM_END(s->list[i]);
        }
        set_moved(anyc ? acc : pre, n0);
        if (!wild) {
            if (t->k == TY_ENUM && ncov < t->ed->nv) die(s->line, "non-exhaustive match: not all variants of '%s' are covered (add '_ =>')", t->ed->name);
            if (t->k == TY_BOOL && ncov < 2) die(s->line, "non-exhaustive match on bool");
            if (is_int(t)) die(s->line, "non-exhaustive match: integer match needs a '_ =>' arm");
        }
        break;
    }
    }
}
static void check_func(Func *f) {
    if (f->is_extern || f->ifc) return;
    cur_fn = f; nlocals = 0; scope_base = 0; cur_off = max_off = 0; loop_depth = 0; in_unsafe = f->is_unsafe; memset(ctor_set, 0, sizeof ctor_set);
    f->plocal = xalloc(sizeof(Local *) * (f->nparams + 1));
    for (int i = 0; i < f->nparams; i++) {
        need_complete(f->pty[i], f->line); require_safe_type(f->pty[i], f->line);
        if (f->pref[i]) { f->plocal[i] = declare(f->pname[i], f->pty[i], 1, 1, f->line); }
        else { f->plocal[i] = declare(f->pname[i], f->pty[i], 0, 0, f->line); if (is_agg(f->pty[i])) f->plocal[i]->tmp = alloc_slot(8, 8); if (f->borrow) f->plocal[i]->dflag = 0; }
    }
    require_safe_type(f->ret, f->line);
    if (is_agg(f->ret)) f->retslot = alloc_slot(8, 8);
    if (f->is_ctor) for (int i = 0; i < f->cls->nf; i++) {
        Field *fl = &f->cls->f[i]; if (!fl->init) continue;
        Stmt dummy; memset(&dummy, 0, sizeof dummy); cur_stmt = &dummy;
        check_expr(fl->init); expect_coerce(fl->init, fl->ty, "field initializer"); consume(fl->init);
        for (Expr *t = dummy.temps; t; t = t->tnext) if (!t->consumed) die(t->line, "this expression creates a temporary with a destructor; assign it to a variable first");
        cur_stmt = 0;
    }
    check_scope_body(f->body);
    if (f->ret->k != TY_VOID && !always_ret(f->body)) die(f->line, "function '%s' may finish without returning a value", f->name);
    f->frame = (max_off + 15) / 16 * 16;
}
static void sema(void) {
    for (int i = 0; i < nallow; i++) for (int j = 0; j < ndeny; j++)
        if (!strcmp(allow_caps[i], deny_caps[j])) die(1, "capability '%s' is both allowed and denied", allow_caps[i]);
    Func *m = 0; for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, "main")) m = funcs[i];
    if (!m && !test_mode) die(1, "no 'main' function");
    if (m && m->is_async) die(m->line, "main cannot be async");
    for (int i = 0; i < nfuncs; i++) if (funcs[i]->is_test && (funcs[i]->nparams || funcs[i]->ret->k != TY_VOID))
        die(funcs[i]->line, "a @test function takes no parameters and returns nothing (fail with panic())");
    for (int i = 0; i < nfuncs; i++) {
        Func *f = funcs[i]; if (!f->is_async) continue;
        if (f->nparams > 5) die(f->line, "async functions can take at most 5 parameters");
        for (int j = 0; j < f->nparams; j++) if (f->pref[j] || has_iface(f->pty[j]))
            die(f->line, "async functions cannot take 'ref' or interface parameters (the task may outlive the caller)");
    }
    if (m && !(m->ret->k == TY_VOID || (is_int(m->ret) && m->ret->size == 4 && m->ret->sign))) die(m->line, "main must return i32");
    int vec_args = m && m->nparams == 1 && !m->pref[0] && !strcmp(tname(m->pty[0]), "Vec<String>");
    if (m && m->nparams != 0 && !vec_args && !(m->nparams == 2 && m->is_unsafe && same(m->pty[0], &ty_i32) && m->pty[1]->k == TY_PTR && m->pty[1]->elem->k == TY_PTR && same(m->pty[1]->elem->elem, &ty_u8)))
        die(m->line, "main must be 'fn main() -> i32', 'fn main(args: Vec<String>) -> i32' or 'unsafe fn main(argc: i32, argv: **u8) -> i32'");
    for (int i = 0; i < nglobals; i++) {
        Local *g = globals[i];
        for (int j = 0; j < i; j++) if (!strcmp(globals[j]->name, g->name)) die(g->line, "global '%s' redefined", g->name);
        if (g->init) {
            Expr *x = g->init; check_expr(x);
            if (!(x->k == E_INT || x->k == E_FLOAT || x->k == E_BOOL || x->k == E_STR || x->k == E_ENUMVAL)) die(g->line, "global initializers must be literals");
            if (g->ty) expect_coerce(x, g->ty, g->is_const ? "constant" : "global initializer");
            else { if (!g->is_const) finalize(x); g->ty = x->ty; }
        }
        need_complete(g->ty, g->line);
        if (g->ty->k == TY_VOID) die(g->line, "global cannot be void");
        if (has_iface(g->ty)) die(g->line, IFACE_MSG, tname(g->ty));
    }
    for (int i = 0; i < nfuncs; i++) if (funcs[i]->is_extern) {
        if (is_agg(funcs[i]->ret)) die(funcs[i]->line, "extern functions cannot return aggregates");
        for (int j = 0; j < funcs[i]->nparams; j++) if (is_agg(funcs[i]->pty[j])) die(funcs[i]->line, "extern functions cannot take aggregates by value");
        int gp = 0, fp = 0, fl = is_float(funcs[i]->ret);
        for (int j = 0; j < funcs[i]->nparams; j++) { if (is_float(funcs[i]->pty[j])) { fp++; fl = 1; } else gp++; }
        if (fl && (gp > 6 || fp > 8)) die(funcs[i]->line, "extern functions with floating-point values can take at most 6 integer and 8 floating-point arguments");
    }
    for (int i = 0; i < nfuncs; i++) {   /* an error ends the check of its function only: the others are still checked */
        jmp_buf jb; err_jb = &jb;
        if (!setjmp(jb)) check_func(funcs[i]);
        else {
            assign_target = 0; cur_stmt = 0;
            if (nerrors >= 20) { fprintf(stderr, "too many errors; stopping\n"); exit(1); }
        }
        err_jb = 0;
    }
    if (nerrors) { if (nerrors > 1) fprintf(stderr, "%d errors\n", nerrors); exit(1); }
    for (int i = 0; i < nstructs; i++) for (int k = 0; k < structs[i]->nifs; k++) check_impl(structs[i], structs[i]->ifs[k], structs[i]->line);
}

/* ============================ code generation ============================ */
typedef struct { char *d; int n; } StrEnt;
static FILE *out; static int lbl_n, push_cnt, ret_lbl;
static int loop_brk[64], loop_cont[64], loop_sp;
static StrEnt *strtab; static int nstr, cstr;

static void emit(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fputc('\t', out); vfprintf(out, fmt, ap); fputc('\n', out); va_end(ap); }
static int newlbl(void) { return lbl_n++; }
static void place(int l) { fprintf(out, ".L%d:\n", l); }
static void gpush(const char *r) { emit("push %s", r); push_cnt++; }
static void gpop(const char *r) { emit("pop %s", r); push_cnt--; }
static int add_str(const char *d, int n) {
    StrEnt s; s.d = xstrn(d, n); s.n = n;
    VPUSH(strtab, nstr, cstr, s); return nstr - 1;
}
static const char *R64[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" }, *R32[] = { "edi", "esi", "edx", "ecx", "r8d", "r9d" },
                  *R16[] = { "di", "si", "dx", "cx", "r8w", "r9w" }, *R8[] = { "dil", "sil", "dl", "cl", "r8b", "r9b" };

static void norm(Type *t) {
    if (t->k != TY_INT && t->k != TY_ENUM) return;
    switch (t->size) {
    case 1: emit(t->sign ? "movsx rax, al" : "movzx eax, al"); break;
    case 2: emit(t->sign ? "movsx rax, ax" : "movzx eax, ax"); break;
    case 4: emit(t->sign ? "movsxd rax, eax" : "mov eax, eax"); break;
    }
}
static void load(Type *t, const char *m) {
    switch (t->k) {
    case TY_BOOL: emit("movzx eax, BYTE PTR %s", m); break;
    case TY_PTR: case TY_STR: emit("mov rax, QWORD PTR %s", m); break;
    default:
        switch (t->size) {
        case 1: emit(t->sign ? "movsx rax, BYTE PTR %s" : "movzx eax, BYTE PTR %s", m); break;
        case 2: emit(t->sign ? "movsx rax, WORD PTR %s" : "movzx eax, WORD PTR %s", m); break;
        case 4: emit(t->sign ? "movsxd rax, DWORD PTR %s" : "mov eax, DWORD PTR %s", m); break;
        default: emit("mov rax, QWORD PTR %s", m);
        }
    }
}
static void store_local(Type *t, int off) {
    switch (t->size) {
    case 1: emit("mov BYTE PTR [rbp-%d], al", off); break;
    case 2: emit("mov WORD PTR [rbp-%d], ax", off); break;
    case 4: emit("mov DWORD PTR [rbp-%d], eax", off); break;
    default: emit("mov QWORD PTR [rbp-%d], rax", off);
    }
}
static void store_ind(Type *t) { /* address in rax, value in rdx */
    switch (t->size) {
    case 1: emit("mov BYTE PTR [rax], dl"); break; case 2: emit("mov WORD PTR [rax], dx"); break;
    case 4: emit("mov DWORD PTR [rax], edx"); break; default: emit("mov QWORD PTR [rax], rdx");
    }
}
static const char *mangle(const char *s) {   /* generic instances contain characters that are not valid in assembler symbols */
    char *o = xalloc(strlen(s) * 4 + 1), *q = o;
    for (; *s; s++) { if (isalnum((unsigned char)*s) || *s == '_' || *s == '.') *q++ = *s; else q += sprintf(q, "_x%02x_", (unsigned char)*s); }
    return o;
}
static void gen_expr(Expr *e); static void gen_stmt(Stmt *s); static void gen_copy(int size); static void xsave(void); static void xrestore(void);
/* ---- floating point: values travel as raw bits in rax like everything else; arithmetic uses xmm0/xmm1 ---- */
typedef struct { char *text; int is32; } FConst;
static FConst *fconsts; static int nfc, cfc;
static void emit_fbits(const char *text, int is32) {   /* exact IEEE bits (strtod/strtof round correctly; the assembler may not) */
    if (is32) { float f = strtof(text, 0); unsigned u; memcpy(&u, &f, 4); fprintf(out, "\t.long 0x%08x\n", u); }
    else { double d = strtod(text, 0); unsigned long long u; memcpy(&u, &d, 8); fprintf(out, "\t.quad 0x%016llx\n", u); }
}
static int add_fconst(const char *text, int is32) { FConst c = { xstrn(text, strlen(text)), is32 }; VPUSH(fconsts, nfc, cfc, c); return nfc - 1; }
static void to_xmm(int x, Type *t, const char *r64) {   /* bits in a GP register -> xmmN */
    if (t->size == 4) emit("movd xmm%d, %s", x, !strcmp(r64, "rax") ? "eax" : "ecx"); else emit("movq xmm%d, %s", x, r64);
}
static void from_xmm0(Type *t) { emit(t->size == 4 ? "movd eax, xmm0" : "movq rax, xmm0"); }
/* ---- destructors: per-type drop functions, drop flags for locals and temporaries ---- */
static Type **glue_ty; static int nglue, cglue;
static void call_drop(Type *t) {   /* object address in rdi */
    char *n = tname(t); int found = 0;
    int Ls = -1;
    if (t->k == TY_STR) {   /* only heap strings need the call: skip null and immortal literals inline */
        Ls = newlbl(); emit("mov rax, QWORD PTR [rdi]"); emit("test rax, rax"); emit("jz .L%d", Ls);
        emit("cmp QWORD PTR [rax-16], 0"); emit("jle .L%d", Ls);
    }
    for (int i = 0; i < nglue; i++) if (!strcmp(tname(glue_ty[i]), n)) found = 1;
    if (!found) VPUSH(glue_ty, nglue, cglue, t);
    int pad = push_cnt & 1; if (pad) emit("sub rsp, 8");
    xsave(); emit("call ag__drop_%s", mangle(n)); xrestore();
    if (pad) emit("add rsp, 8");
    if (Ls >= 0) place(Ls);
}
static Type **copy_ty; static int ncopy, ccopy;
static void call_copy(Type *t) {   /* object address in rdi: increment the reference counts it holds */
    char *n = tname(t); int found = 0;
    for (int i = 0; i < ncopy; i++) if (!strcmp(tname(copy_ty[i]), n)) found = 1;
    if (!found) VPUSH(copy_ty, ncopy, ccopy, t);
    int pad = push_cnt & 1; if (pad) emit("sub rsp, 8");
    xsave(); emit("call ag__copy_%s", mangle(n)); xrestore();
    if (pad) emit("add rsp, 8");
}
static void str_retain(const char *r) {   /* String pointer in register r: one more owner */
    int L = newlbl();
    emit("test %s, %s", r, r); emit("jz .L%d", L); emit("cmp QWORD PTR [%s-16], 0", r); emit("jl .L%d", L);
    emit("add QWORD PTR [%s-16], 1", r); place(L);
}
static Local *dl[4096]; static int ndl, dscope[512], ndscope, loop_dl[64];
static Stmt *cur_gstmt;
static void drop_local(Local *l) { int L = newlbl(); emit("cmp BYTE PTR [rbp-%d], 0", l->dflag); emit("je .L%d", L); emit("lea rdi, [rbp-%d]", l->off); call_drop(l->ty); place(L); }
static void drops_from(int from) { for (int i = ndl - 1; i >= from; i--) drop_local(dl[i]); }
static Local *xl[4096]; static int nxl, xscope[512];   /* float register variables currently in scope (saved around calls) */
static void scope_open(void) { xscope[ndscope] = nxl; dscope[ndscope++] = ndl; }
static void scope_close(void) { int b = dscope[--ndscope]; drops_from(b); ndl = b; nxl = xscope[ndscope]; }
static void reg_local(Local *l) { emit("mov BYTE PTR [rbp-%d], 1", l->dflag); dl[ndl++] = l; }
static void xlive(Local *l) { if (l->reg >= 8 && nxl < 4096) xl[nxl++] = l; }
static void temps_zero(Stmt *s) { for (Expr *t = s->temps; t; t = t->tnext) emit("mov BYTE PTR [rbp-%d], 0", t->dflag); }
static void temps_drop(Stmt *s) {
    for (Expr *t = s->temps; t; t = t->tnext) {
        int L = newlbl(); emit("cmp BYTE PTR [rbp-%d], 0", t->dflag); emit("je .L%d", L);
        emit("mov BYTE PTR [rbp-%d], 0", t->dflag); emit("lea rdi, [rbp-%d]", t->tmp); call_drop(t->ty); place(L);
    }
}
static void temps_drop_keep(Stmt *s) {   /* same, preserving rax */
    if (!s->temps) return;
    gpush("rax"); gpush("rax"); temps_drop(s); gpop("rax"); gpop("rax");
}

/* ============================ optimizer (-O) ============================
 * Works on the typed AST right before code generation:
 *  - constant folding
 *  - register promotion: scalar locals whose address is never taken live in registers
 *    (rbx, r12-r15 for integers/pointers - callee-saved; xmm8-xmm15 for floats - saved around calls)
 *  - better instruction selection in the generator (immediates, direct operands, compare-and-branch, addressing modes)
 */
static int opt, gp_busy[6], gp_used[6];
static const char *GREG[] = { "", "rbx", "r12", "r13", "r14", "r15" };
static const char *GREG32[] = { "", "ebx", "r12d", "r13d", "r14d", "r15d" };
static Local *olocs[4096]; static int nolocs, ostamp, oloopw, f_has_xmm;
static int is_scalar(Type *t) { return t->k == TY_INT || t->k == TY_ENUM || t->k == TY_BOOL || t->k == TY_PTR || t->k == TY_STR || t->k == TY_FLOAT; }
static void oa_note(Local *l) {
    if (!l || l->is_global) return;
    if (l->seen != ostamp) { l->seen = ostamp; l->uses = 0; l->addr_taken = 0; l->reg = 0; l->has_range = 0; l->is_cst = 0; l->nasg = 0; l->ifirst = 0; l->ilast = 1 << 30; if (nolocs < 4096) olocs[nolocs++] = l; }
}
static long long fold_norm(long long v, Type *t) {   /* wrap to the type's width, keep the canonical sign/zero extension */
    if (t->k == TY_BOOL) return v != 0;
    switch (t->size) {
    case 1: return t->sign ? (long long)(signed char)v : (long long)(unsigned char)v;
    case 2: return t->sign ? (long long)(short)v : (long long)(unsigned short)v;
    case 4: return t->sign ? (long long)(int)v : (long long)(unsigned)v;
    }
    return v;
}
static int fconst_val(Expr *e, double *v) {   /* value of a float constant node */
    if (!e->ty || !is_float(e->ty)) return 0;
    if (e->k == E_INT && e->cls == 0) { *v = e->ty->size == 4 ? (double)(float)e->ival : (double)e->ival; return 1; }
    if (e->k == E_FLOAT && e->sval) { *v = e->ty->size == 4 ? (double)strtof(e->sval, 0) : strtod(e->sval, 0); return 1; }
    return 0;
}
static void set_fconst(Expr *e, double v) {
    char b[64]; snprintf(b, sizeof b, e->ty->size == 4 ? "%.9g" : "%.17g", v);
    e->k = E_FLOAT; e->sval = xstrn(b, strlen(b)); e->cls = 0; e->a = e->b = 0;
}
static void fold(Expr *e) {   /* constant folding on integer expressions (in place) */
    if (!e) return;
    if (e->k == E_VAR && e->lv && e->lv->is_cst && e->lv->seen == ostamp) { e->k = E_INT; e->ival = e->lv->cval; e->cls = 0; e->lv = 0; return; }
    if ((e->k == E_REF || (e->k == E_UN && e->op == O_ADDR)) && e->a->k == E_VAR) return;   /* places stay places */
    fold(e->a); fold(e->b);
    for (int i = 0; i < e->nargs; i++) fold(e->args[i]);
    if (e->k == E_CAST && e->ty && is_float(e->ty) && e->a->k == E_INT && e->a->cls == 0 && is_int(e->a->ty) && (e->a->ty->sign || e->a->ival >= 0)
        && e->a->ival > -(1LL << 53) && e->a->ival < (1LL << 53)) { e->k = E_INT; e->ival = e->a->ival; e->cls = 0; return; }   /* exact as a float constant */
    if (e->ty && is_float(e->ty)) {   /* exact IEEE arithmetic on constants, same rounding as the hardware */
        double a, b, r; int f32 = e->ty->size == 4;
        if (e->k == E_BIN && e->op >= O_ADD && e->op <= O_DIV && fconst_val(e->a, &a) && fconst_val(e->b, &b) && same(e->a->ty, e->ty) && same(e->b->ty, e->ty)) {
            if (f32) { float x = (float)a, y = (float)b, z = e->op == O_ADD ? x + y : e->op == O_SUB ? x - y : e->op == O_MUL ? x * y : x / y; r = z; }
            else r = e->op == O_ADD ? a + b : e->op == O_SUB ? a - b : e->op == O_MUL ? a * b : a / b;
            if (r == r) set_fconst(e, r);
        } else if (e->k == E_UN && e->op == O_NEG && fconst_val(e->a, &a) && same(e->a->ty, e->ty)) set_fconst(e, -a);
        else if (e->k == E_CAST && fconst_val(e->a, &a)) { r = f32 ? (double)(float)a : a; if (r == r) set_fconst(e, r); }
        return;
    }
    if (!e->ty || (!is_int(e->ty) && e->ty->k != TY_BOOL)) return;
    if (e->k == E_UN && e->a->k == E_INT && is_int(e->a->ty) && (e->op == O_NEG || e->op == O_BNOT)) {
        long long v = e->a->ival; v = e->op == O_NEG ? (long long)(0ULL - (unsigned long long)v) : ~v;
        e->k = E_INT; e->ival = fold_norm(v, e->ty); e->cls = 0; return;
    }
    if (e->k == E_UN && e->op == O_NOT && e->a->k == E_BOOL) { e->k = E_BOOL; e->ival = !e->a->ival; return; }
    if (e->k != E_BIN || e->a->k != E_INT || e->b->k != E_INT || !is_int(e->a->ty) || !is_int(e->b->ty)) return;
    unsigned long long a = (unsigned long long)e->a->ival, b = (unsigned long long)e->b->ival; long long r; int sg = e->a->ty->sign, cmp = 0;
    switch (e->op) {
    case O_ADD: r = (long long)(a + b); break; case O_SUB: r = (long long)(a - b); break; case O_MUL: r = (long long)(a * b); break;
    case O_DIV: case O_MOD:
        if (b == 0 || (sg && (long long)b == -1)) return;
        if (sg) r = e->op == O_DIV ? (long long)a / (long long)b : (long long)a % (long long)b; else r = (long long)(e->op == O_DIV ? a / b : a % b);
        break;
    case O_AND: r = (long long)(a & b); break; case O_OR: r = (long long)(a | b); break; case O_XOR: r = (long long)(a ^ b); break;
    case O_SHL: r = (long long)(a << (b & 63)); break;
    case O_SHR: r = sg ? (long long)a >> (b & 63) : (long long)(a >> (b & 63)); break;
    case O_EQ: r = a == b; cmp = 1; break; case O_NE: r = a != b; cmp = 1; break;
    case O_LT: r = sg ? (long long)a < (long long)b : a < b; cmp = 1; break; case O_LE: r = sg ? (long long)a <= (long long)b : a <= b; cmp = 1; break;
    case O_GT: r = sg ? (long long)a > (long long)b : a > b; cmp = 1; break; case O_GE: r = sg ? (long long)a >= (long long)b : a >= b; cmp = 1; break;
    default: return;
    }
    if (cmp) { e->k = E_BOOL; e->ival = r; return; }
    e->k = E_INT; e->ival = fold_norm(r, e->ty); e->cls = 0;
}
static void oa_expr(Expr *e) {
    if (!e) return;
    if (e->k == E_VAR && e->lv && !e->lv->is_global) { oa_note(e->lv); e->lv->uses += oloopw; }
    if (e->k == E_UN && e->op == O_ADDR && e->a->k == E_VAR && e->a->lv) { oa_note(e->a->lv); e->a->lv->addr_taken = 1; }
    if (e->k == E_REF && e->a->k == E_VAR && e->a->lv && !e->a->lv->is_ref) { oa_note(e->a->lv); e->a->lv->addr_taken = 1; }
    oa_expr(e->a); oa_expr(e->b);
    for (int i = 0; i < e->nargs; i++) oa_expr(e->args[i]);
}
static Local *mk_bound_local(Stmt *s) { Local *l = xalloc(sizeof *l); l->name = "for.bound"; l->ty = s->e2->ty->sign ? &ty_i64 : &ty_u64; l->off = s->slot1; return l; }   /* 8-byte slot */
static int opos; static Local *odecl[4096]; static int nodecl;
static void odeclare(Local *l) { l->ifirst = opos; if (nodecl < 4096) odecl[nodecl++] = l; }
static void oscope_end(int mark) { for (int i = mark; i < nodecl; i++) odecl[i]->ilast = opos; nodecl = mark; }
static int for_bound_var(Stmt *s) {   /* the bound is an immutable 8-byte local: compare against it directly */
    Expr *b = s->e2; return b->k == E_VAR && b->lv && !b->lv->is_global && !b->lv->is_mut && !b->lv->is_ref && b->lv->ty->size == 8;
}
static void oa_stmt(Stmt *s) {
    if (!s) return;
    opos++;
    switch (s->k) {
    case S_BLOCK: { int mark = nodecl; for (int i = 0; i < s->n; i++) oa_stmt(s->list[i]); opos++; oscope_end(mark); break; }
    case S_LET:
        fold(s->e); oa_expr(s->e); oa_note(s->lv); s->lv->uses += oloopw; odeclare(s->lv);
        if (!s->is_mut && s->e && s->e->k == E_INT && s->e->cls == 0 && is_int(s->lv->ty) && !s->lv->dflag) { s->lv->is_cst = 1; s->lv->cval = s->e->ival; }
        break;
    case S_ASSIGN:
        fold(s->e); fold(s->e2); oa_expr(s->e); oa_expr(s->e2);
        if (s->e->k == E_VAR && s->e->lv) { oa_note(s->e->lv); s->e->lv->nasg++; }
        break;
    case S_EXPR: fold(s->e); oa_expr(s->e); break;
    case S_RET: fold(s->e); oa_expr(s->e); break;
    case S_IF: fold(s->e); oa_expr(s->e); oa_stmt(s->body); oa_stmt(s->els); break;
    case S_WHILE: { int w = oloopw; if (oloopw < (1 << 20)) oloopw *= 8; fold(s->e); oa_expr(s->e); oa_stmt(s->body); oloopw = w; break; }
    case S_FOR: {
        int w = oloopw; fold(s->e); fold(s->e2); oa_expr(s->e); oa_expr(s->e2);
        if (oloopw < (1 << 20)) oloopw *= 8;
        int mark = nodecl;
        oa_note(s->lv); s->lv->uses += oloopw; odeclare(s->lv);
        int cimm = s->is_range && s->e2->k == E_INT && s->e2->ival >= -2147483647LL && s->e2->ival <= 2147483647LL;
        if (s->is_range && !cimm && !for_bound_var(s)) { if (!s->bnd) s->bnd = mk_bound_local(s); s->bnd->seen = 0; oa_note(s->bnd); s->bnd->uses += oloopw; odeclare(s->bnd); }
        oa_stmt(s->body); oloopw = w; opos++; oscope_end(mark); break;
    }
    case S_MATCH:
        fold(s->e); oa_expr(s->e);
        for (int i = 0; i < s->n; i++) {
            int mark = nodecl;
            if (s->alv && s->alv[i]) { oa_note(s->alv[i]); odeclare(s->alv[i]); }
            if (s->vlv && s->vlv[i]) for (int j = 0; j < MAXVF; j++) if (s->vlv[i][j]) { oa_note(s->vlv[i][j]); odeclare(s->vlv[i][j]); }
            oa_stmt(s->list[i]); opos++; oscope_end(mark);
        }
        break;
    default: break;
    }
}
static void opt_analyze(Func *f) {
    nolocs = 0; ostamp++; oloopw = 1; opos = 0; nodecl = 0; f_has_xmm = 0; f->nsaved = 0; memset(gp_busy, 0, sizeof gp_busy); memset(gp_used, 0, sizeof gp_used);
    for (int i = 0; i < f->nparams; i++) { oa_note(f->plocal[i]); f->plocal[i]->uses += 1; }
    if (f->is_ctor) for (int i = 0; i < f->cls->nf; i++) if (f->cls->f[i].init) { fold(f->cls->f[i].init); oa_expr(f->cls->f[i].init); }
    oa_stmt(f->body);
    Local *cand[4096]; int nc = 0;
    for (int i = 0; i < nolocs; i++) {
        Local *l = olocs[i];
        if (l->addr_taken || l->dflag || l->uses < 2) continue;
        if (l->is_ref || is_scalar(l->ty)) cand[nc++] = l;
    }
    for (int i = 1; i < nc; i++) {   /* most used (loop-weighted) first; stable */
        Local *l = cand[i]; int j = i - 1;
        while (j >= 0 && cand[j]->uses < l->uses) { cand[j + 1] = cand[j]; j--; }
        cand[j + 1] = l;
    }
    /* registers are shared by locals whose scopes do not overlap */
    static Local *onreg[16][4096]; int non[16] = { 0 };
    for (int i = 0; i < nc; i++) {
        Local *l = cand[i]; int fl = !l->is_ref && is_float(l->ty);
        for (int r = fl ? 8 : 1; r <= (fl ? 15 : 5); r++) {
            int ok = 1;
            for (int j = 0; j < non[r] && ok; j++) { Local *o = onreg[r][j]; if (!(o->ilast < l->ifirst || l->ilast < o->ifirst)) ok = 0; }
            if (!ok || non[r] >= 4096) continue;
            onreg[r][non[r]++] = l; l->reg = r;
            if (fl) f_has_xmm = 1;
            else if (!gp_used[r]) { f->saved[f->nsaved++] = (char)r; gp_busy[r] = gp_used[r] = 1; }
            break;
        }
    }
}
/* register-aware access to locals; the value travels in rax like everywhere else */
static void load_var(Local *l) {
    if (l->reg >= 8) emit(l->ty->size == 4 ? "movd eax, xmm%d" : "movq rax, xmm%d", l->reg);
    else if (l->reg) emit("mov rax, %s", GREG[l->reg]);
    else { char m[32]; snprintf(m, sizeof m, "[rbp-%d]", l->off); load(l->is_ref ? &ty_u64 : l->ty, m); }
}
static void store_var(Local *l) {
    if (l->reg >= 8) emit(l->ty->size == 4 ? "movd xmm%d, eax" : "movq xmm%d, rax", l->reg);
    else if (l->reg) emit("mov %s, rax", GREG[l->reg]);
    else store_local(l->is_ref ? &ty_u64 : l->ty, l->off);
}
static void xsave(void) {   /* calls clobber all xmm registers: park promoted floats in their home slots */
    if (!opt || !f_has_xmm) return;
    for (int i = 0; i < nxl; i++) { Local *l = xl[i]; emit(l->ty->size == 4 ? "movss DWORD PTR [rbp-%d], xmm%d" : "movsd QWORD PTR [rbp-%d], xmm%d", l->off, l->reg); }
}
static void xrestore(void) {
    if (!opt || !f_has_xmm) return;
    for (int i = 0; i < nxl; i++) { Local *l = xl[i]; emit(l->ty->size == 4 ? "movss xmm%d, DWORD PTR [rbp-%d]" : "movsd xmm%d, QWORD PTR [rbp-%d]", l->reg, l->off); }
}

/* ---- instruction selection helpers (-O) ---- */
static const char *GREG8[] = { "", "bl", "r12b", "r13b", "r14b", "r15b" }, *GREG16[] = { "", "bx", "r12w", "r13w", "r14w", "r15w" };
static const char *SCR[] = { "r8", "r9", "r10", "r11" };
static int scr_depth;
static int tmp_gp(void) { for (int r = 1; r <= 5; r++) if (!gp_busy[r]) { gp_busy[r] = gp_used[r] = 1; return r; } return 0; }   /* survives calls */
static int imm32(long long v) { return v >= -2147483648LL && v <= 2147483647LL; }
static int is_intlike(Type *t) { return t->k == TY_INT || t->k == TY_ENUM || t->k == TY_BOOL || t->k == TY_PTR || t->k == TY_STR; }
static Expr *try_inline(Expr *e); static int simple_int(Expr *e, char *buf); static int lv_static(Expr *e); static int lv_nocode(Expr *e); static int fsimple(Expr *e, char *buf);
static int fbase, need_fneg, no_froute;
static const char *ptr_kw(int size);
static int has_call(Expr *e) {   /* could evaluating e call a function (clobbering scratch registers)? */
    if (!e) return 0;
    if (e->k == E_CALL) {
        if (e->builtin == 7 || e->builtin == 8 || e->builtin == 9) return has_call(e->args[0]);   /* take(p), s.len(), cstr: inline */
        Expr *b = try_inline(e); if (b) return has_call(b);
        if (opt && e->fn && e->fn->is_extern && !strcmp(e->fn->name, "sqrt") && e->nargs == 1 && e->ty->size == 8 && fbase < 7) return has_call(e->args[0]);
    }
    switch (e->k) {
    case E_CALL: case E_AWAIT: case E_TRY: case E_OK: case E_ERR: case E_TOIFACE: case E_MCALL: case E_VARIANT: return 1;
    default: break;
    }
    if (has_call(e->a) || has_call(e->b)) return 1;
    for (int i = 0; i < e->nargs; i++) if (has_call(e->args[i])) return 1;
    return 0;
}
/* ---- inlining (-O): calls to small functions whose body is a single 'return expr' are replaced by that
 * expression with the parameters substituted (a ref parameter becomes the place passed by the caller). ---- */
static int inl_depth;
static int inl_nodes(Expr *e, Func *f, int *n) {   /* can this callee expression be copied into a caller? */
    if (!e) return 1;
    if (++*n > 40) return 0;
    switch (e->k) {
    case E_INT: case E_FLOAT: case E_BOOL: case E_ENUMVAL: case E_STR: break;
    case E_VAR: if (!e->lv) return 0; break;
    case E_BIN: case E_CAST: case E_FIELD: case E_INDEX: break;
    case E_UN: if (e->op == O_ADDR) return 0; break;
    case E_CALL:
        if (e->builtin || !e->fn || e->fn == f || e->tmp || e->dflag || e->fn->is_async || e->fn->ifc) return 0;
        for (int i = 0; i < e->nargs; i++) if (e->args[i]->k == E_REF && !(e->args[i]->a->k == E_VAR && e->args[i]->a->lv->is_ref)) {
            Expr *x = e->args[i]->a; while (x->k == E_FIELD || x->k == E_INDEX) x = x->a;
            if (x->k == E_VAR && !x->lv->is_ref && !x->lv->is_global) return 0;   /* would take the address of a parameter */
        }
        break;
    case E_REF: break;
    default: return 0;
    }
    if (e->k == E_VAR || e->k == E_REF) { if (e->k == E_REF && !inl_nodes(e->a, f, n)) return 0; return 1; }
    if (!inl_nodes(e->a, f, n) || !inl_nodes(e->b, f, n)) return 0;
    for (int i = 0; i < e->nargs; i++) if (!inl_nodes(e->args[i], f, n)) return 0;
    return 1;
}
static Expr *inl_body(Func *f) {
    if (f->inl_state) return f->inl_state == 1 ? f->inl_body : 0;
    f->inl_state = 2;
    if (f->is_extern || f->ifc || f->is_async || f->is_ctor || f->is_dtor || !f->body || is_agg(f->ret) || f->ret->k == TY_VOID || f->nparams > 6) return 0;
    for (int i = 0; i < f->nparams; i++) if (!f->pref[i] && (is_agg(f->pty[i]) || has_drop(f->pty[i]))) return 0;
    if (has_drop(f->ret)) return 0;
    Stmt *b = f->body;
    while (b->k == S_BLOCK && b->n == 1 && b->list[0]->k == S_BLOCK) b = b->list[0];   /* unsafe { return ...; } */
    if (b->k != S_BLOCK || b->n != 1 || b->list[0]->k != S_RET || !b->list[0]->e || b->list[0]->temps) return 0;
    int n = 0; Expr *e = b->list[0]->e;
    if (!inl_nodes(e, f, &n)) return 0;
    f->inl_state = 1; f->inl_body = e; return e;
}
static Expr *inl_clone(Expr *e, Func *f, Expr **args) {
    if (!e) return 0;
    if (e->k == E_VAR) for (int i = 0; i < f->nparams; i++) if (e->lv == f->plocal[i]) return f->pref[i] ? args[i]->a : args[i];
    Expr *c = xalloc(sizeof *c); *c = *e; c->inl = 0; c->inl_done = 0;
    c->a = inl_clone(e->a, f, args); c->b = inl_clone(e->b, f, args);
    if (e->nargs) { c->args = xalloc(sizeof(Expr *) * (size_t)e->nargs); for (int i = 0; i < e->nargs; i++) c->args[i] = inl_clone(e->args[i], f, args); }
    return c;
}
static int cheap_arg(Expr *a, int pref) {   /* fine to evaluate more than once */
    char t[96];
    if (pref) { Expr *x = a->a; return lv_nocode(x) || lv_static(x); }
    return simple_int(a, t) || (a->ty && is_float(a->ty) && fsimple(a, 0));
}
static int count_uses(Expr *e, Local *l) {
    if (!e) return 0;
    int n = e->k == E_VAR && e->lv == l;
    n += count_uses(e->a, l) + count_uses(e->b, l);
    for (int i = 0; i < e->nargs; i++) n += count_uses(e->args[i], l);
    return n;
}
static Expr *try_inline(Expr *e) {
    if (!opt || e->k != E_CALL || e->builtin || !e->fn) return 0;
    if (e->inl_done) return e->inl;
    e->inl_done = 1;
    if (inl_depth > 4 || e->dflag || e->tmp) return 0;
    Func *f = e->fn; Expr *body = inl_body(f);
    if (!body || e->nargs != f->nparams) return 0;
    for (int i = 0; i < e->nargs; i++) {
        Expr *a = e->args[i];
        if (has_call(a) || a->move || a->dflag) return 0;
        if (f->pref[i] && a->k != E_REF) return 0;
        if (f->pref[i] && !same(a->a->ty, f->pty[i])) return 0;
        int u = count_uses(body, f->plocal[i]);
        if (u != 1 && !cheap_arg(a, f->pref[i])) return 0;   /* evaluated more than once, or not at all (keep its traps) */
    }
    e->inl = inl_clone(body, f, e->args);
    fold(e->inl);
    return e->inl;
}
static int regvar(Expr *e) { return e->k == E_VAR && e->lv && e->lv->reg && e->lv->reg < 8 && !e->lv->is_ref ? e->lv->reg : 0; }
/* a cheap integer expression that can be an instruction operand directly: immediate, register variable or 8-byte stack slot */
static int simple_int(Expr *e, char *buf) {
    if (!opt || !e->ty || !is_intlike(e->ty)) return 0;
    if ((e->k == E_INT && e->cls != 1) || e->k == E_BOOL || e->k == E_ENUMVAL || (e->k == E_INT && e->cls == 1)) {
        if (e->k == E_INT && e->cls == 2) { strcpy(buf, "0"); return 1; }   /* null */
        if (!imm32(e->ival)) return 0;
        sprintf(buf, "%lld", e->ival); return 1;
    }
    if (regvar(e)) { strcpy(buf, GREG[regvar(e)]); return 1; }
    if (e->k == E_VAR && e->lv && !e->lv->is_global && !e->lv->is_ref && !e->lv->reg && e->ty->size == 8 && !is_agg(e->ty)) { sprintf(buf, "QWORD PTR [rbp-%d]", e->lv->off); return 1; }
    return 0;
}
static int is_unsigned_cmp(Type *t);
static const char *cc_of(int op, int u, int neg) {   /* condition code for a comparison, optionally negated */
    static const char *s[6] = { "e", "ne", "l", "le", "g", "ge" }, *us[6] = { "e", "ne", "b", "be", "a", "ae" }, *ns[6] = { "ne", "e", "ge", "g", "le", "l" }, *nus[6] = { "ne", "e", "ae", "a", "be", "b" };
    int i = op - O_EQ; return neg ? (u ? nus[i] : ns[i]) : (u ? us[i] : s[i]);
}
static int log2_exact(long long v) { if (v <= 0 || (v & (v - 1))) return -1; int k = 0; while ((1LL << k) != v) k++; return k; }
static void norm_reg(Type *t, int r) {   /* canonical form of a register variable after arithmetic */
    if (t->k != TY_INT && t->k != TY_ENUM) return;
    switch (t->size) {
    case 1: emit(t->sign ? "movsx %s, %s" : "movzx %s, %s", t->sign ? GREG[r] : GREG32[r], GREG8[r]); break;
    case 2: emit(t->sign ? "movsx %s, %s" : "movzx %s, %s", t->sign ? GREG[r] : GREG32[r], GREG16[r]); break;
    case 4: if (t->sign) emit("movsxd %s, %s", GREG[r], GREG32[r]); else emit("mov %s, %s", GREG32[r], GREG32[r]); break;
    }
}
/* second operand of a binary operation: an operand string, using a scratch register or the stack when needed */
static void gen_rhs(Expr *b, char *rb) {
    if (simple_int(b, rb)) return;
    int t;
    if (!has_call(b) && scr_depth < 4) {
        const char *sr = SCR[scr_depth++]; emit("mov %s, rax", sr); gen_expr(b); emit("mov rcx, rax"); emit("mov rax, %s", sr); scr_depth--;
    } else if ((t = tmp_gp())) { emit("mov %s, rax", GREG[t]); gen_expr(b); emit("mov rcx, rax"); emit("mov rax, %s", GREG[t]); gp_busy[t] = 0; }
    else { gpush("rax"); gen_expr(b); emit("mov rcx, rax"); gpop("rax"); }
    strcpy(rb, "rcx");
}
/* integer binary operation, left in rax, right operand rb: result in rax */
static int nonorm;   /* the next int_binop cannot wrap (value range known): skip re-normalisation */
static void int_binop(int op, Type *t, Type *rt0, const char *rb, Expr *b) {
    Type *rt = nonorm ? &ty_i64 : rt0; nonorm = 0;
    long long iv = 0; int isimm = b && (b->k == E_INT || b->k == E_BOOL || b->k == E_ENUMVAL) && imm32(b->ival);
    if (isimm) iv = b->ival;
    switch (op) {
    case O_ADD: emit("add rax, %s", rb); norm(rt); break;
    case O_SUB: emit("sub rax, %s", rb); norm(rt); break;
    case O_MUL:
        if (isimm && log2_exact(iv) >= 0) { if (iv > 1) emit("shl rax, %d", log2_exact(iv)); }
        else if (isimm) emit("imul rax, rax, %lld", iv); else emit("imul rax, %s", rb);
        norm(rt); break;
    case O_DIV: case O_MOD: {
        int k = isimm ? log2_exact(iv) : -1;
        if (k >= 0) {   /* power of two */
            if (!t->sign) { if (op == O_DIV) { if (k) emit("shr rax, %d", k); } else if (k < 31) emit("and eax, %lld", iv - 1); else { emit("mov rcx, %lld", iv - 1); emit("and rax, rcx"); } }
            else if (op == O_DIV) { if (k) { emit("mov rcx, rax"); emit("sar rcx, 63"); emit("shr rcx, %d", 64 - k); emit("add rax, rcx"); emit("sar rax, %d", k); } }
            else if (k == 0) emit("xor eax, eax");
            else { emit("mov rcx, rax"); emit("sar rcx, 63"); emit("shr rcx, %d", 64 - k); emit("add rcx, rax"); emit("and rcx, %lld", -iv); emit("sub rax, rcx"); }
            norm(rt); break;
        }
        if (strcmp(rb, "rcx")) emit("mov rcx, %s", rb);
        if (t->sign) { emit("cqo"); emit("idiv rcx"); } else { emit("xor edx, edx"); emit("div rcx"); }
        if (op == O_MOD) emit("mov rax, rdx");
        norm(rt); break;
    }
    case O_AND: emit("and rax, %s", rb); break; case O_OR: emit("or rax, %s", rb); break; case O_XOR: emit("xor rax, %s", rb); break;
    case O_SHL: if (isimm) emit("shl rax, %lld", iv & 63); else { if (strcmp(rb, "rcx")) emit("mov rcx, %s", rb); emit("shl rax, cl"); } norm(rt); break;
    case O_SHR:
        if (isimm) emit(t->sign ? "sar rax, %lld" : "shr rax, %lld", iv & 63);
        else { if (strcmp(rb, "rcx")) emit("mov rcx, %s", rb); emit(t->sign ? "sar rax, cl" : "shr rax, cl"); }
        break;
    default: emit("cmp rax, %s", rb); emit("set%s al", cc_of(op, is_unsigned_cmp(t), 0)); emit("movzx eax, al");
    }
}
/* value range of an integer expression (no wrap-around possible inside the range); 0 = unknown */
static int irange(Expr *e, long long *lo, long long *hi) {
    if (!e || !e->ty || !is_int(e->ty)) return 0;
    const long long LIM = 1LL << 40;
    long long a, b, c, d;
    switch (e->k) {
    case E_INT: if (e->cls != 0) return 0; *lo = *hi = e->ival; return 1;
    case E_VAR: if (!e->lv || !e->lv->has_range) return 0; *lo = e->lv->lo; *hi = e->lv->hi; return 1;
    case E_CAST: if (!is_int(e->a->ty) || !irange(e->a, lo, hi)) return 0; break;
    case E_BIN:
        if (!irange(e->a, &a, &b)) return 0;
        if (e->op == O_MOD || e->op == O_DIV || e->op == O_AND || e->op == O_SHR) {
            if (e->b->k != E_INT || e->b->ival <= 0) return 0;
            long long m = e->b->ival;
            if (e->op == O_MOD) { *lo = a >= 0 ? 0 : -(m - 1); *hi = b < 0 ? 0 : (b < m - 1 && a >= 0 ? b : m - 1); }
            else if (e->op == O_DIV) { *lo = a / m; *hi = b / m; }
            else if (e->op == O_AND) { if (a < 0) return 0; *lo = 0; *hi = b < m ? b : m; }
            else { if (a < 0 || m > 62) return 0; *lo = a >> m; *hi = b >> m; }
            break;
        }
        if (!irange(e->b, &c, &d)) return 0;
        if (a < -LIM || b > LIM || c < -LIM || d > LIM) return 0;
        if (e->op == O_ADD) { *lo = a + c; *hi = b + d; }
        else if (e->op == O_SUB) { *lo = a - d; *hi = b - c; }
        else if (e->op == O_MUL) {
            if (a < -(1LL << 20) || b > (1LL << 20) || c < -(1LL << 20) || d > (1LL << 20)) return 0;
            long long p[4] = { a * c, a * d, b * c, b * d }; *lo = *hi = p[0];
            for (int i = 1; i < 4; i++) { if (p[i] < *lo) *lo = p[i]; if (p[i] > *hi) *hi = p[i]; }
        } else return 0;
        break;
    default: return 0;
    }
    /* the result must fit the type, otherwise the operation wrapped */
    Type *t = e->ty;
    if (!t->sign && *lo < 0) return 0;
    if (t->size < 8) { long long mx = t->sign ? (1LL << (t->size * 8 - 1)) - 1 : (1LL << (t->size * 8)) - 1, mn = t->sign ? -mx - 1 : 0; if (*lo < mn || *hi > mx) return 0; }
    return 1;
}
static int in_bounds(Expr *ix, long long n) { long long lo, hi; return opt && irange(ix, &lo, &hi) && lo >= 0 && hi < n; }
/* ---- addresses as x86 memory operands (-O) ---- */
typedef struct { char base[64]; char idx[8]; int scale; long long disp; } Addr;
static void addr_str(Addr *a, char *buf) {
    int n = sprintf(buf, "[%s", a->base);
    if (a->idx[0]) n += sprintf(buf + n, "+%s*%d", a->idx, a->scale);
    if (a->disp > 0) n += sprintf(buf + n, "+%lld", a->disp); else if (a->disp < 0) n += sprintf(buf + n, "%lld", a->disp);
    strcpy(buf + n, "]");
}
static int lv_static(Expr *e); static int lv_cheap(Expr *e);
static const char *lv_tmp = "rax";   /* register used to load a 'ref' slot */
static int ptr_base(Expr *e, Addr *a, const char *tmp) {   /* base register holding a pointer value */
    if (regvar(e)) { strcpy(a->base, GREG[regvar(e)]); return 1; }
    char rb[48]; if (simple_int(e, rb)) { emit("mov %s, %s", tmp, rb); strcpy(a->base, tmp); return 1; }
    return 0;
}
/* Address of an lvalue without a pointer in rax. May clobber rax and rcx (never other registers, never calls)
   unless 'any' is set, in which case arbitrary index code may run first. Returns 0 if the shape is not handled. */
static int lvaddr(Expr *e, Addr *a, int any) {
    memset(a, 0, sizeof *a); a->scale = 1;
    switch (e->k) {
    case E_VAR: {
        Local *l = e->lv;
        if (l->is_global) { sprintf(a->base, "rip+ag_g_%s", l->name); return 1; }
        if (l->is_ref) { if (l->reg) strcpy(a->base, GREG[l->reg]); else { emit("mov %s, QWORD PTR [rbp-%d]", lv_tmp, l->off); strcpy(a->base, lv_tmp); } return 1; }
        if (l->reg) return 0;
        strcpy(a->base, "rbp"); a->disp = -l->off; return 1;
    }
    case E_FIELD:
        if (e->a->ty->k == TY_PTR) { if (!ptr_base(e->a, a, "rax")) return 0; a->disp = e->fld->off; return 1; }
        if (!lv_static(e->a) && !(e->a->k == E_INDEX && (any || lv_cheap(e->a)))) return 0;
        if (!lvaddr(e->a, a, any)) return 0;
        a->disp += e->fld->off; return 1;
    case E_UN:
        if (e->op != O_DEREF) return 0;
        return ptr_base(e->a, a, "rax");
    case E_INDEX: {
        Type *bt = e->a->ty; int sz = e->ty->size, arr = bt->k == TY_ARRAY;
        if (!arr && bt->k != TY_PTR) return 0;
        Expr *ix = e->b;
        if (arr && !lv_static(e->a)) return 0;
        if (!arr && !regvar(e->a) && !simple_int(e->a, (char[48]){0})) return 0;
        if (ix->k == E_INT && ix->cls == 0) {   /* constant index */
            if (arr && (unsigned long long)ix->ival >= (unsigned long long)bt->n) { emit("jmp .Ltrap"); }
            if (arr) { if (!lvaddr(e->a, a, 0)) return 0; } else if (!ptr_base(e->a, a, "rcx")) return 0;
            a->disp += ix->ival * sz; return 1;
        }
        int ir = regvar(ix);
        if (!ir && !any && has_call(ix)) return 0;
        if (!ir && !any) {   /* only cheap index code allowed: nothing but rax/rcx may change */
            char t[48]; if (!simple_int(ix, t)) return 0;
        }
        const char *idx = ir ? GREG[ir] : "rax";
        if (!ir) gen_expr(ix);
        int checked = arr && !in_bounds(ix, bt->n);
        if (checked) { emit("cmp %s, %lld", idx, bt->n); emit("jae .Ltrap"); }
        if (sz != 1 && sz != 2 && sz != 4 && sz != 8) { emit("imul rax, %s, %d", idx, sz); idx = "rax"; sz = 1; }
        if (arr) {
            Addr b; const char *sv = lv_tmp; lv_tmp = "rcx"; int ok = lvaddr(e->a, &b, 0); lv_tmp = sv; if (!ok) return 0;
            if (!strncmp(b.base, "rip+", 4)) memmove(b.base, b.base + 4, strlen(b.base) - 3);   /* non-PIE: absolute address + index */
            *a = b;
        } else if (!ptr_base(e->a, a, "rcx")) return 0;
        strcpy(a->idx, idx); a->scale = sz; return 1;
    }
    default: return 0;
    }
}
static int lv_static(Expr *e) {   /* address needs no code (or only rax for a ref slot) and has no index register */
    switch (e->k) {
    case E_VAR: return !e->lv->reg || e->lv->is_ref;
    case E_FIELD: return e->a->ty->k != TY_PTR ? lv_static(e->a) : regvar(e->a) != 0;
    case E_INDEX: return e->a->ty->k == TY_ARRAY && e->b->k == E_INT && e->b->cls == 0 && lv_static(e->a);
    default: return 0;
    }
}
static int lv_cheap(Expr *e) {
    switch (e->k) {
    case E_VAR: return !e->lv->reg || e->lv->is_ref;
    case E_FIELD: return e->a->ty->k == TY_PTR ? (regvar(e->a) || simple_int(e->a, (char[48]){0})) : lv_cheap(e->a);
    case E_UN: return e->op == O_DEREF && (regvar(e->a) || simple_int(e->a, (char[48]){0}));
    case E_INDEX: {
        Type *bt = e->a->ty; char t[48];
        if (bt->k == TY_ARRAY) { if (!lv_static(e->a)) return 0; }
        else if (bt->k == TY_PTR) { if (!regvar(e->a) && !simple_int(e->a, t)) return 0; }
        else return 0;
        return (e->b->k == E_INT && e->b->cls == 0) || simple_int(e->b, t);
    }
    default: return 0;
    }
}

static int has_float(Expr *e) {   /* any floating point (xmm) work inside? */
    if (!e) return 0;
    if (e->ty && is_float(e->ty)) return 1;
    if (e->k == E_CAST && e->a->ty && is_float(e->a->ty)) return 1;
    if (has_float(e->a) || has_float(e->b)) return 1;
    for (int i = 0; i < e->nargs; i++) if (has_float(e->args[i])) return 1;
    return 0;
}
static int can_fpe(Expr *e) {   /* could raise a hardware divide error */
    if (!e) return 0;
    if (e->k == E_BIN && (e->op == O_DIV || e->op == O_MOD) && e->ty && is_int(e->ty) && !(e->b->k == E_INT && e->b->ival != 0 && e->b->ival != -1)) return 1;
    if (can_fpe(e->a) || can_fpe(e->b)) return 1;
    for (int i = 0; i < e->nargs; i++) if (can_fpe(e->args[i])) return 1;
    return 0;
}
/* lvaddr(e, a, 1) will succeed; the index code touches only integer registers (rax, rcx, rdx, r8-r11), no xmm, no calls */
static int lv_anyok(Expr *e) {
    char t[48];
    switch (e->k) {
    case E_VAR: return e->lv->is_global || e->lv->is_ref || !e->lv->reg;
    case E_FIELD:
        if (e->a->ty->k == TY_PTR) return regvar(e->a) || simple_int(e->a, t);
        return lv_static(e->a) || (e->a->k == E_INDEX && lv_anyok(e->a));
    case E_UN: return e->op == O_DEREF && (regvar(e->a) || simple_int(e->a, t));
    case E_INDEX: {
        Type *bt = e->a->ty;
        if (bt->k == TY_ARRAY) { if (!lv_static(e->a)) return 0; }
        else if (bt->k == TY_PTR) { if (!regvar(e->a) && !simple_int(e->a, t)) return 0; }
        else return 0;
        return !has_call(e->b) && !has_float(e->b);
    }
    default: return 0;
    }
}
/* ---- floating point in xmm registers (-O) ----
 * gen_fexpr(e, k) leaves the value in xmm<k>. xmm0-7 are scratch: an expression evaluated at level k>0 never
 * contains a call (callers spill to the stack otherwise), so lower scratch registers survive. Register variables
 * live in xmm8-15 and are parked in their stack slots around calls. */
static int fnative(Expr *e) {   /* kinds gen_fexpr handles itself (everything else goes through rax) */
    switch (e->k) {
    case E_FLOAT: case E_INT: case E_VAR: case E_FIELD: case E_INDEX: case E_BIN: return 1;
    case E_UN: return e->op == O_NEG || e->op == O_DEREF;
    case E_CAST: return 1;
    case E_CALL: return (e->fn && e->fn->is_extern && !strcmp(e->fn->name, "sqrt") && e->nargs == 1 && e->ty->size == 8) || try_inline(e) != 0;
    default: return 0;
    }
}
static int lv_ripbase(Expr *e);
static void lvaddr_must(Expr *e, Addr *a) { if (!lvaddr(e, a, 0)) { fprintf(stderr, "internal error: lvaddr\n"); exit(3); } }
static void lvaddr_any(Expr *e, Addr *a) { if (!lvaddr(e, a, 1)) { fprintf(stderr, "internal error: lvaddr(any)\n"); exit(3); } }
static int lv_nocode(Expr *e) {   /* address operand with no code at all */
    switch (e->k) {
    case E_VAR: return e->lv->is_ref ? e->lv->reg != 0 : !e->lv->reg;
    case E_FIELD: return e->a->ty->k != TY_PTR ? lv_nocode(e->a) : regvar(e->a) != 0;
    case E_INDEX:
        if (e->a->ty->k != TY_ARRAY || !lv_nocode(e->a)) return 0;
        if (e->b->k == E_INT && e->b->cls == 0) return (unsigned long long)e->b->ival < (unsigned long long)e->a->ty->n;
        return regvar(e->b) && in_bounds(e->b, e->a->ty->n);
    default: return 0;
    }
}
static int lv_ripbase(Expr *e) { while (e->k == E_FIELD || e->k == E_INDEX) { if (e->k == E_FIELD && e->a->ty->k == TY_PTR) return 0; e = e->a; } return e->k == E_VAR && e->lv->is_global; }
/* operand for the second source of an SSE instruction; buf == 0 only checks */
static int fsimple(Expr *e, char *buf) {
    int f32 = e->ty->size == 4;
    if (e->k == E_FLOAT || (e->k == E_INT && is_float(e->ty))) {
        if (buf) { char tx[40]; const char *t = e->sval; if (e->k == E_INT) { snprintf(tx, sizeof tx, "%lld", e->ival); t = tx; } sprintf(buf, "%s PTR [rip+.LF%d]", f32 ? "DWORD" : "QWORD", add_fconst(t, f32)); }
        return 1;
    }
    if (e->k == E_VAR && e->lv->reg >= 8) { if (buf) sprintf(buf, "xmm%d", e->lv->reg); return 1; }
    if ((e->k == E_VAR || e->k == E_FIELD || e->k == E_INDEX) && lv_nocode(e)) {
        if (buf) { Addr a; char m[96]; lvaddr_must(e, &a); addr_str(&a, m); sprintf(buf, "%s PTR %s", f32 ? "DWORD" : "QWORD", m); }
        return 1;
    }
    return 0;
}
static void gen_fexpr(Expr *e, int k);
static void fop(int op, int f32, int k, const char *src) {
    static const char *mn[] = { "add", "sub", "mul", "div" };
    emit("%s%s xmm%d, %s", mn[op - O_ADD], f32 ? "ss" : "sd", k, src);
}
static int is_ftwo(Expr *e) { return (e->k == E_INT && e->ival == 2 && e->cls == 0) || (e->k == E_FLOAT && e->sval && strtod(e->sval, 0) == 2.0); }
static void gen_fexpr(Expr *e, int k) {
    int f32 = e->ty->size == 4; char src[160];
    switch (e->k) {
    case E_FLOAT: case E_INT: fsimple(e, src); emit("%s xmm%d, %s", f32 ? "movss" : "movsd", k, src); return;
    case E_VAR:
        if (e->lv->reg >= 8) { emit("movaps xmm%d, xmm%d", k, e->lv->reg); return; }
        /* fallthrough */
    case E_FIELD: case E_INDEX: {
        Addr a; char m[96]; int fb = fbase, ok; fbase = k; ok = lvaddr(e, &a, 1); fbase = fb;   /* index code may use xmm<k> and up */
        if (ok) { addr_str(&a, m); emit("%s xmm%d, %s PTR %s", f32 ? "movss" : "movsd", k, f32 ? "DWORD" : "QWORD", m); return; }
        break;
    }
    case E_UN:
        if (e->op == O_NEG) { gen_fexpr(e->a, k); need_fneg = 1; emit(f32 ? "xorps xmm%d, XMMWORD PTR [rip+.Lfneg32]" : "xorpd xmm%d, XMMWORD PTR [rip+.Lfneg64]", k); return; }
        if (e->op == O_DEREF) { Addr a; char m[96]; int fb = fbase, ok; fbase = k; ok = lvaddr(e, &a, 1); fbase = fb; if (ok) { addr_str(&a, m); emit("%s xmm%d, %s PTR %s", f32 ? "movss" : "movsd", k, f32 ? "DWORD" : "QWORD", m); return; } }
        break;
    case E_CAST: {
        Type *from = e->a->ty;
        if (is_float(from)) { gen_fexpr(e->a, k); if (from->size != e->ty->size) emit(f32 ? "cvtsd2ss xmm%d, xmm%d" : "cvtss2sd xmm%d, xmm%d", k, k); return; }
        int fb = fbase; fbase = k; gen_expr(e->a); fbase = fb;
        const char *cv = f32 ? "cvtsi2ss" : "cvtsi2sd";
        emit("pxor xmm%d, xmm%d", k, k);
        if (from->size == 8 && !from->sign) {
            int L1 = newlbl(), L2 = newlbl();
            emit("test rax, rax"); emit("js .L%d", L1); emit("%s xmm%d, rax", cv, k); emit("jmp .L%d", L2);
            place(L1); emit("mov rcx, rax"); emit("shr rcx, 1"); emit("and eax, 1"); emit("or rcx, rax"); emit("%s xmm%d, rcx", cv, k);
            emit(f32 ? "addss xmm%d, xmm%d" : "addsd xmm%d, xmm%d", k, k); place(L2);
        } else emit("%s xmm%d, rax", cv, k);
        return;
    }
    case E_CALL: {   /* sqrt intrinsic, inlined calls */
        if (fnative(e) && e->fn->is_extern) { gen_fexpr(e->args[0], k); emit("sqrtsd xmm%d, xmm%d", k, k); return; }
        Expr *ib = try_inline(e);
        if (ib) { inl_depth++; gen_fexpr(ib, k); inl_depth--; return; }
        break;
    }
    case E_BIN: {
        int op = e->op; Expr *L = e->a, *R = e->b;
        if (op < O_ADD || op > O_DIV) break;
        if (op == O_MUL && (is_ftwo(L) || is_ftwo(R))) {   /* 2*x == x+x exactly */
            gen_fexpr(is_ftwo(L) ? R : L, k); emit(f32 ? "addss xmm%d, xmm%d" : "addsd xmm%d, xmm%d", k, k); return;
        }
        if ((op == O_ADD || op == O_MUL) && fsimple(L, 0) && !fsimple(R, 0) && !has_call(R)) { Expr *t = L; L = R; R = t; }
        gen_fexpr(L, k);
        if (fsimple(R, 0)) { fsimple(R, src); fop(op, f32, k, src); return; }
        if ((R->k == E_INDEX || R->k == E_FIELD || (R->k == E_UN && R->op == O_DEREF) || (R->k == E_VAR && !R->lv->reg)) && lv_anyok(R)) {
            Addr a; char m[96]; lvaddr_any(R, &a); addr_str(&a, m); sprintf(src, "%s PTR %s", f32 ? "DWORD" : "QWORD", m); fop(op, f32, k, src); return;
        }
        if (!has_call(R) && k < 6) { gen_fexpr(R, k + 1); sprintf(src, "xmm%d", k + 1); fop(op, f32, k, src); return; }
        int tg = tmp_gp(), t = k == 7 ? 6 : 7;
        if (tg) {   /* keep the left value in a callee-saved register across the call */
            emit(f32 ? "movd %s, xmm%d" : "movq %s, xmm%d", f32 ? GREG32[tg] : GREG[tg], k); gen_fexpr(R, k);
            emit(f32 ? "movd xmm%d, %s" : "movq xmm%d, %s", t, f32 ? GREG32[tg] : GREG[tg]); gp_busy[tg] = 0;
            sprintf(src, "xmm%d", k); fop(op, f32, t, src); emit("movaps xmm%d, xmm%d", k, t); return;
        }
        emit("sub rsp, 16"); push_cnt += 2; emit(f32 ? "movss DWORD PTR [rsp], xmm%d" : "movsd QWORD PTR [rsp], xmm%d", k);   /* spill the left value across a call */
        gen_fexpr(R, k);
        emit("%s xmm%d, %s PTR [rsp]", f32 ? "movss" : "movsd", t, f32 ? "DWORD" : "QWORD");
        sprintf(src, "xmm%d", k); fop(op, f32, t, src); emit("movaps xmm%d, xmm%d", k, t);
        emit("add rsp, 16"); push_cnt -= 2;
        return;
    }
    default: break;
    }
    int fb = fbase; fbase = k; no_froute = 1; gen_expr(e); fbase = fb;   /* anything else produces its bits in rax */
    emit(f32 ? "movd xmm%d, eax" : "movq xmm%d, rax", k);
}
static int reads_xreg(Expr *e, int r) {
    if (!e) return 0;
    if (e->k == E_VAR && e->lv && e->lv->reg == r && !e->lv->is_ref) return 1;
    if (reads_xreg(e->a, r) || reads_xreg(e->b, r)) return 1;
    for (int i = 0; i < e->nargs; i++) if (reads_xreg(e->args[i], r)) return 1;
    return 0;
}
/* float expression straight into register variable xmm<dst> (no copy through xmm0); call-free expressions only */
static void gen_fexpr_to(Expr *e, int dst) {
    int f32 = e->ty->size == 4; char src[160];
    if (fsimple(e, 0)) { fsimple(e, src); if (src[0] == 'x') { if (atoi(src + 3) != dst) emit("movaps xmm%d, %s", dst, src); } else emit("%s xmm%d, %s", f32 ? "movss" : "movsd", dst, src); return; }
    if (!has_call(e) && e->k == E_BIN && e->op >= O_ADD && e->op <= O_DIV && !is_ftwo(e->a) && !is_ftwo(e->b) && fnative(e->a)) {
        Expr *L = e->a, *R = e->b; int op = e->op;
        if ((op == O_ADD || op == O_MUL) && (fsimple(L, 0) || reads_xreg(R, dst)) && !reads_xreg(L, dst) && fnative(R)) { Expr *t = L; L = R; R = t; }
        if (!reads_xreg(R, dst)) {
            if (fsimple(R, 0)) { gen_fexpr_to(L, dst); fsimple(R, src); fop(op, f32, dst, src); return; }
            if ((R->k == E_INDEX || R->k == E_FIELD || (R->k == E_UN && R->op == O_DEREF) || (R->k == E_VAR && !R->lv->reg)) && lv_anyok(R)) {
                Addr a; char m[96]; gen_fexpr_to(L, dst); lvaddr_any(R, &a); addr_str(&a, m); sprintf(src, "%s PTR %s", f32 ? "DWORD" : "QWORD", m); fop(op, f32, dst, src); return;
            }
            gen_fexpr_to(L, dst); gen_fexpr(R, 0); fop(op, f32, dst, "xmm0"); return;
        }
    }
    gen_fexpr(e, 0); emit("movaps xmm%d, xmm0", dst);
}
/* float comparison: L in xmm<k>, R in xmm<k+1> */
static void fcmp_operands(Expr *L, Expr *R, int k) {
    gen_fexpr(L, k);
    if (!has_call(R) && k < 6) { gen_fexpr(R, k + 1); return; }
    int f32 = L->ty->size == 4;
    emit("sub rsp, 16"); push_cnt += 2; emit(f32 ? "movss DWORD PTR [rsp], xmm%d" : "movsd QWORD PTR [rsp], xmm%d", k);
    gen_fexpr(R, k + 1); emit("%s xmm%d, %s PTR [rsp]", f32 ? "movss" : "movsd", k, f32 ? "DWORD" : "QWORD");
    emit("add rsp, 16"); push_cnt -= 2;
}
static void fbranch(Expr *e, int L, int when) {   /* NaN compares false (except !=) */
    int k = fbase, f32 = e->a->ty->size == 4; const char *uc = f32 ? "ucomiss" : "ucomisd";
    int op = e->op; char src[160];
    {   /* one side needs no code: compare a register with it directly (X > Y / X >= Y / X == Y) */
        Expr *X = 0, *Y = 0;
        if (op == O_GT || op == O_GE || op == O_EQ || op == O_NE) { if (fsimple(e->b, 0)) { X = e->a; Y = e->b; } }
        if (op == O_LT || op == O_LE || op == O_EQ || op == O_NE) { if (!X && fsimple(e->a, 0)) { X = e->b; Y = e->a; } }
        if (X) {
            int xr = X->k == E_VAR && X->lv->reg >= 8 ? X->lv->reg : k;
            if (xr == k) gen_fexpr(X, k);
            fsimple(Y, src);
            emit("%s xmm%d, %s", uc, xr, src);
            if (op == O_EQ || op == O_NE) {
                if ((op == O_EQ) == when) { int Ls = newlbl(); emit("jp .L%d", Ls); emit("je .L%d", L); place(Ls); }
                else { emit("jne .L%d", L); emit("jp .L%d", L); }
            } else { int eq = op == O_LE || op == O_GE; emit(when ? (eq ? "jae .L%d" : "ja .L%d") : (eq ? "jb .L%d" : "jbe .L%d"), L); }
            return;
        }
    }
    fcmp_operands(e->a, e->b, k);
    if (op == O_EQ || op == O_NE) {
        emit("%s xmm%d, xmm%d", uc, k, k + 1);
        if ((op == O_EQ) == when) { int Ls = newlbl(); emit("jp .L%d", Ls); emit("je .L%d", L); place(Ls); }
        else { emit("jne .L%d", L); emit("jp .L%d", L); }
        return;
    }
    int swap = op == O_LT || op == O_LE, eq = op == O_LE || op == O_GE;
    emit("%s xmm%d, xmm%d", uc, swap ? k + 1 : k, swap ? k : k + 1);
    emit(when ? (eq ? "jae .L%d" : "ja .L%d") : (eq ? "jb .L%d" : "jbe .L%d"), L);
}
static void fcmp_value(Expr *e) {
    int k = fbase, f32 = e->a->ty->size == 4; const char *uc = f32 ? "ucomiss" : "ucomisd";
    fcmp_operands(e->a, e->b, k);
    int op = e->op;
    if (op == O_EQ) { emit("%s xmm%d, xmm%d", uc, k, k + 1); emit("sete al"); emit("setnp cl"); emit("and al, cl"); }
    else if (op == O_NE) { emit("%s xmm%d, xmm%d", uc, k, k + 1); emit("setne al"); emit("setp cl"); emit("or al, cl"); }
    else if (op == O_LT) { emit("%s xmm%d, xmm%d", uc, k + 1, k); emit("seta al"); }
    else if (op == O_LE) { emit("%s xmm%d, xmm%d", uc, k + 1, k); emit("setae al"); }
    else if (op == O_GT) { emit("%s xmm%d, xmm%d", uc, k, k + 1); emit("seta al"); }
    else { emit("%s xmm%d, xmm%d", uc, k, k + 1); emit("setae al"); }
    emit("movzx eax, al");
}
/* jump to L when e's truth value equals 'when' */
static void gen_branch(Expr *e, int L, int when) {
    if (e->k == E_BOOL) { if ((e->ival != 0) == when) emit("jmp .L%d", L); return; }
    if (e->k == E_UN && e->op == O_NOT) { gen_branch(e->a, L, !when); return; }
    if (e->k == E_BIN && (e->op == O_LAND || e->op == O_LOR)) {
        int both = (e->op == O_LAND) == !when;   /* LAND jumping on false / LOR jumping on true: either side decides */
        if (both) { gen_branch(e->a, L, when); gen_branch(e->b, L, when); }
        else { int Ls = newlbl(); gen_branch(e->a, Ls, !when); gen_branch(e->b, L, when); place(Ls); }
        return;
    }
    if (e->k == E_BIN && e->op >= O_EQ && e->op <= O_GE && is_intlike(e->a->ty)) {
        char rb[48]; int lr = regvar(e->a), z = e->b->k == E_INT && e->b->ival == 0;
        int bimm = (e->b->k == E_INT || e->b->k == E_BOOL || e->b->k == E_ENUMVAL) && imm32(e->b->ival);
        if (lr && z) emit("test %s, %s", GREG[lr], GREG[lr]);
        else if (lr && simple_int(e->b, rb)) emit("cmp %s, %s", GREG[lr], rb);
        else if (bimm && e->a->ty->size <= 8 && (e->a->k == E_VAR || e->a->k == E_FIELD || e->a->k == E_INDEX || (e->a->k == E_UN && e->a->op == O_DEREF)) && !regvar(e->a) && lv_nocode(e->a)) {
            Addr a; char m[96]; int sz = e->a->ty->size; lvaddr_must(e->a, &a); addr_str(&a, m);
            emit("cmp %s PTR %s, %lld", ptr_kw(sz), m, sz == 1 ? (e->b->ival & 255) : sz == 2 ? (e->b->ival & 65535) : sz == 4 ? (long long)(int)e->b->ival : e->b->ival);
        }
        else { gen_expr(e->a); if (z) emit("test rax, rax"); else { gen_rhs(e->b, rb); emit("cmp rax, %s", rb); } }
        emit("j%s .L%d", cc_of(e->op, is_unsigned_cmp(e->a->ty), !when), L);
        return;
    }
    if (e->k == E_BIN && e->op >= O_EQ && e->op <= O_GE && is_float(e->a->ty) && fbase < 6) { fbranch(e, L, when); return; }
    if (regvar(e)) { emit("test %s, %s", GREG[regvar(e)], GREG[regvar(e)]); emit(when ? "jnz .L%d" : "jz .L%d", L); return; }
    if (e->ty && e->ty->k == TY_BOOL && lv_nocode(e)) { Addr a; char m[96]; lvaddr_must(e, &a); addr_str(&a, m); emit("cmp BYTE PTR %s, 0", m); emit(when ? "jne .L%d" : "je .L%d", L); return; }
    gen_expr(e); emit("test rax, rax"); emit(when ? "jnz .L%d" : "jz .L%d", L);
}

static void gen_addr(Expr *e) {
    switch (e->k) {
    case E_VAR:
        if (e->lv->reg) { if (!e->lv->is_ref) { fprintf(stderr, "internal error: address of a register variable\n"); exit(3); } emit("mov rax, %s", GREG[e->lv->reg]); break; }
        if (e->lv->is_global) emit("lea rax, [rip+ag_g_%s]", e->lv->name);
        else if (e->lv->is_ref) emit("mov rax, QWORD PTR [rbp-%d]", e->lv->off); else emit("lea rax, [rbp-%d]", e->lv->off);
        break;
    case E_INDEX: {
        Type *bt = e->a->ty; gen_expr(e->a); gpush("rax"); gen_expr(e->b);
        if (bt->k == TY_ARRAY) { emit("cmp rax, %lld", bt->n); emit("jae .Ltrap"); }
        if (bt->k == TY_STR) {   /* bounds-checked byte of a String (null is empty) */
            gpop("rcx"); emit("test rcx, rcx"); emit("jz .Ltrap"); emit("cmp rax, QWORD PTR [rcx-8]"); emit("jae .Ltrap");
            emit("add rax, rcx"); break;
        }
        if (e->ty->size != 1) emit("imul rax, rax, %d", e->ty->size);
        gpop("rcx"); emit("add rax, rcx"); break;
    }
    case E_FIELD: gen_expr(e->a); if (e->fld->off) emit("add rax, %d", e->fld->off); break;
    case E_UN: gen_expr(e->a); break;   /* deref */
    default:
        if (is_agg(e->ty)) { gen_expr(e); break; }   /* calls, Ok/Err, '?' yield the address of a temporary */
        fprintf(stderr, "internal error: gen_addr\n"); exit(3);
    }
}
static int arg_simple(Expr *a, Type *pt, int pref, char *buf, int *kind) {   /* argument loadable with one instruction */
    Addr ad; char m[96];
    if (a->move || a->dflag || a->copy) return 0;   /* ownership bookkeeping happens in gen_expr */
    if (pref || is_agg(pt)) {   /* address of a place */
        Expr *x = pref ? (a->k == E_REF ? a->a : 0) : a;
        if (!x || !lv_nocode(x)) return 0;
        lvaddr_must(x, &ad); addr_str(&ad, buf); *kind = 1; return 1;
    }
    if (is_float(pt)) {
        if (a->k == E_VAR && a->lv->reg >= 8) { sprintf(buf, "xmm%d", a->lv->reg); *kind = pt->size == 4 ? 3 : 2; return 1; }
        if (!fsimple(a, 0)) return 0;
        fsimple(a, m); strcpy(buf, m); *kind = pt->size == 4 ? 5 : 4; return 1;
    }
    if (simple_int(a, buf)) { *kind = 0; return 1; }
    if (a->k == E_BIN && (a->op == O_ADD || a->op == O_SUB) && regvar(a->a) && a->b->k == E_INT && a->b->cls == 0 && imm32(a->b->ival) && imm32(-a->b->ival) && is_int(a->ty)) {
        long long lo, hi;
        if (a->ty->size == 8 || irange(a, &lo, &hi)) { long long d = a->op == O_ADD ? a->b->ival : -a->b->ival; sprintf(buf, "[%s%+lld]", GREG[regvar(a->a)], d); *kind = 1; return 1; }
    }
    return 0;
}
static int gen_call_opt(Expr *e) {
    Func *f = e->fn; int n = e->nargs, hid = !f->is_async && is_agg(f->ret);
    if (f->ifc || e->builtin == 3 || n + hid > 6 || n > 8) return 0;
    if (f->is_extern) { if (is_float(f->ret)) return 0; for (int i = 0; i < n; i++) if (is_float(f->pty[i])) return 0; }
    int simple[8], kind[8], pushed[8]; char sb[8][128];
    for (int i = 0; i < n; i++) { simple[i] = arg_simple(e->args[i], f->pty[i], f->pref[i], sb[i], &kind[i]); pushed[i] = 0; }
    for (int i = n - 1; i >= 0; i--) {   /* complex arguments, right to left as always */
        if (simple[i]) continue;
        Expr *a = e->args[i]; int dest = i + hid, later = 0;
        if (a->k == E_REF) gen_addr(a->a); else gen_expr(a);
        for (int j = 0; j < i; j++) if (!simple[j] && has_call(e->args[j])) later = 1;
        if (!later && (!strcmp(R64[dest], "rdi") || !strcmp(R64[dest], "rsi"))) emit("mov %s, rax", R64[dest]);   /* nothing call-free touches rdi/rsi */
        else { gpush("rax"); pushed[i] = 1; }
    }
    for (int i = 0; i < n; i++) if (pushed[i]) gpop(R64[i + hid]);
    for (int i = 0; i < n; i++) {
        if (!simple[i]) continue;
        const char *d = R64[i + hid], *d32 = R32[i + hid];
        switch (kind[i]) {
        case 0: if (!strcmp(sb[i], "0")) emit("xor %s, %s", d32, d32); else emit("mov %s, %s", d, sb[i]); break;
        case 1: emit("lea %s, %s", d, sb[i]); break;
        case 2: emit("movq %s, %s", d, sb[i]); break;
        case 3: emit("movd %s, %s", d32, sb[i]); break;
        case 4: emit("mov %s, %s", d, sb[i]); break;
        case 5: emit("mov %s, %s", d32, sb[i]); break;
        }
    }
    if (hid) emit("lea rdi, [rbp-%d]", e->tmp);
    if (f->is_extern) emit("xor eax, eax");
    int pad = push_cnt & 1; if (pad) emit("sub rsp, 8");
    xsave();
    if (f->is_async) emit("call ag__spawn_%s", mangle(f->name)); else emit("call %s%s", f->is_extern ? "" : "ag_", mangle(f->name));
    xrestore();
    if (pad) emit("add rsp, 8");
    if (f->is_extern) norm(f->ret);
    if (f->is_async) { emit("mov QWORD PTR [rbp-%d], rax", e->tmp); emit("lea rax, [rbp-%d]", e->tmp); }
    return 1;
}
static void gen_call(Expr *e) {
    if (opt && gen_call_opt(e)) return;
    Func *f = e->fn; int ctor = e->builtin == 3, n = e->nargs, hid = !f->is_async && (is_agg(f->ret) || ctor), tot = n + hid, sa = tot > 6 ? tot - 6 : 0;
    if (ctor) { emit("lea rdi, [rbp-%d]", e->tmp); emit("xor eax, eax"); emit("mov ecx, %d", f->cls->ty->size); emit("rep stosb"); }   /* zeroed object, 'this' = its address */
    int pad = (push_cnt + sa) & 1; if (pad) { emit("sub rsp, 8"); push_cnt++; }
    for (int i = n - 1; i >= 0; i--) {
        Expr *a = e->args[i];
        if (i == 0 && f->ifc) {   /* interface call: this = object address, target = vtable[slot] (kept in r11) */
            gen_expr(a); emit("mov r10, rax"); emit("push QWORD PTR [r10]"); push_cnt++;
            emit("mov r11, QWORD PTR [r10+8]"); emit("mov r11, QWORD PTR [r11+%d]", 8 * f->vt_idx); continue;
        }
        if (a->k == E_REF) gen_addr(a->a); else gen_expr(a);
        gpush("rax");
    }
    if (hid) { emit("lea rax, [rbp-%d]", e->tmp); gpush("rax"); }
    int fx = f->is_extern && is_float(f->ret), nfp = 0;
    for (int i = 0; i < f->nparams; i++) if (f->is_extern && is_float(f->pty[i])) fx = 1;
    if (fx) {   /* System V: float arguments in xmm0-7, others in rdi.., al = number of vector registers */
        int gp = 0;
        for (int i = 0; i < n; i++) {
            if (is_float(f->pty[i])) { gpop("rax"); to_xmm(nfp++, f->pty[i], "rax"); }
            else gpop(R64[gp++]);
        }
        emit("mov eax, %d", nfp);
    } else {
        for (int i = 0; i < tot && i < 6; i++) gpop(R64[i]);
        if (f->is_extern) emit("xor eax, eax");
    }
    xsave();
    if (f->ifc) emit("call r11");
    else if (f->is_async) emit("call ag__spawn_%s", mangle(f->name));   /* starts the task, returns its handle */
    else emit("call %s%s", f->is_extern ? "" : "ag_", mangle(f->name));
    xrestore();
    if (sa + pad) { emit("add rsp, %d", 8 * (sa + pad)); push_cnt -= sa + pad; }
    if (f->is_extern && is_float(f->ret)) from_xmm0(f->ret);
    else if (f->is_extern) norm(f->ret);
    if (ctor) emit("lea rax, [rbp-%d]", e->tmp);
    if (f->is_async) { emit("mov QWORD PTR [rbp-%d], rax", e->tmp); emit("lea rax, [rbp-%d]", e->tmp); }
}
static void gen_print(Expr *e) {
    Expr *a = e->args[0]; Type *t = a->ty; const char *fmt; gen_expr(a);
    if (t->k == TY_STR) { emit("mov rsi, rax"); emit("lea rcx, [rip+.Lempty]"); emit("test rsi, rsi"); emit("cmovz rsi, rcx"); fmt = "%s"; }
    else if (t->k == TY_BOOL) { emit("lea rsi, [rip+.Lfalse]"); emit("lea rcx, [rip+.Ltrue]"); emit("test rax, rax"); emit("cmovne rsi, rcx"); fmt = "%s"; }
    else if (is_float(t)) { to_xmm(0, t, "rax"); if (t->size == 4) emit("cvtss2sd xmm0, xmm0"); fmt = "%.15g"; }
    else if (t->is_char) { emit("mov rsi, rax"); fmt = "%c"; }
    else { emit("mov rsi, rax"); fmt = t->sign ? "%lld" : "%llu"; }
    char buf[16]; snprintf(buf, sizeof buf, "%s%s", fmt, e->builtin == 2 ? "\n" : "");
    emit("lea rdi, [rip+.LS%d]", add_str(buf, (int)strlen(buf)));
    int pad = push_cnt & 1; if (pad) emit("sub rsp, 8");
    emit(is_float(t) ? "mov eax, 1" : "xor eax, eax"); xsave(); emit("call printf"); xrestore(); if (pad) emit("add rsp, 8");
}
static int is_unsigned_cmp(Type *t) { return (t->k == TY_INT && !t->sign) || t->k == TY_PTR || t->k == TY_BOOL; }

static void gen_expr0(Expr *e);
static void gen_expr(Expr *e) {
    gen_expr0(e);
    if (e->copy) {   /* a copied place: count the new owner */
        if (e->ty->k == TY_STR) str_retain("rax");
        else { gpush("rax"); emit("mov rdi, rax"); call_copy(e->ty); gpop("rax"); }
    }
    if (e->dflag && !is_agg(e->ty)) emit("mov QWORD PTR [rbp-%d], rax", e->tmp);   /* scalar temporary: kept for its drop */
    if (e->dflag) emit("mov BYTE PTR [rbp-%d], 1", e->dflag);   /* temporary now alive */
    if (e->move && e->lv->dflag) emit("mov BYTE PTR [rbp-%d], 0", e->lv->dflag);   /* moved out: no drop at scope end */
}
static void gen_expr0(Expr *e) {
    int nofr = no_froute; no_froute = 0;   /* gen_fexpr fell back to the generic path for this node */
    if (opt && !nofr && e->ty && is_float(e->ty) && fnative(e) && fbase < 7) {
        gen_fexpr(e, fbase); emit(e->ty->size == 4 ? "movd eax, xmm%d" : "movq rax, xmm%d", fbase); return;
    }
    if (opt && e->k == E_BIN && e->op >= O_EQ && e->op <= O_GE && is_float(e->a->ty) && fbase < 6) { fcmp_value(e); return; }
    if (opt && e->k == E_CAST && is_float(e->a->ty) && is_intlike(e->ty) && fbase < 7) {   /* float -> integer */
        gen_fexpr(e->a, fbase); emit(e->a->ty->size == 8 ? "cvttsd2si rax, xmm%d" : "cvttss2si rax, xmm%d", fbase); norm(e->ty); return;
    }
    switch (e->k) {
    case E_INT: case E_ENUMVAL: case E_BOOL:
        if (e->k == E_INT && is_float(e->ty)) {   /* integer literal used as a float */
            char tx[32]; snprintf(tx, sizeof tx, "%lld", e->ival); int c = add_fconst(tx, e->ty->size == 4);
            emit(e->ty->size == 4 ? "mov eax, DWORD PTR [rip+.LF%d]" : "mov rax, QWORD PTR [rip+.LF%d]", c); break;
        }
        if (e->ival == 0) emit("xor eax, eax"); else emit("mov rax, %lld", e->ival); break;
    case E_FLOAT: { int c = add_fconst(e->sval, e->ty->size == 4); emit(e->ty->size == 4 ? "mov eax, DWORD PTR [rip+.LF%d]" : "mov rax, QWORD PTR [rip+.LF%d]", c); break; }
    case E_STR: emit("lea rax, [rip+.LS%d]", add_str(e->sval, e->slen)); break;
    case E_VAR: case E_INDEX: case E_FIELD:
        if (e->k == E_VAR && e->lv->reg && !e->lv->is_ref) { load_var(e->lv); break; }
        if (opt && !is_agg(e->ty) && e->k != E_VAR) { Addr a; char m[96]; if (lvaddr(e, &a, 1)) { addr_str(&a, m); load(e->ty, m); break; } }
        if (e->k == E_VAR && !e->lv->is_ref && !e->lv->is_global && !is_agg(e->ty)) { char m[32]; snprintf(m, sizeof m, "[rbp-%d]", e->lv->off); load(e->ty, m); break; }
        gen_addr(e); if (!is_agg(e->ty)) load(e->ty, "[rax]"); break;
    case E_REF: gen_addr(e->a); break;
    case E_VARIANT: {   /* tag + values in the expression's temporary */
        VarInfo *v = &e->ty->ed->vi[e->ival];
        for (int i = 0; i < e->nargs; i++) {
            Type *ft = v->ft[i]; gen_expr(e->args[i]);
            if (is_agg(ft)) { emit("mov rsi, rax"); emit("lea rdi, [rbp-%d]", e->tmp - v->off[i]); gen_copy(ft->size); }
            else store_local(ft, e->tmp - v->off[i]);
        }
        emit("mov QWORD PTR [rbp-%d], %lld", e->tmp, e->ival);
        emit("lea rax, [rbp-%d]", e->tmp); break;
    }
    case E_SLIT:   /* fill the temporary field by field */
        for (int i = 0; i < e->nargs; i++) {
            Expr *fi = e->args[i]; Field *f = fi->fld; gen_expr(fi->a);
            if (is_agg(f->ty)) { emit("mov rsi, rax"); emit("lea rdi, [rbp-%d]", e->tmp - f->off); gen_copy(f->ty->size); }
            else store_local(f->ty, e->tmp - f->off);
        }
        emit("lea rax, [rbp-%d]", e->tmp); break;
    case E_CALL: {
        Expr *ib = try_inline(e);
        if (ib) { inl_depth++; gen_expr(ib); inl_depth--; break; }
    }
        if (e->builtin == 1 || e->builtin == 2) gen_print(e);
        else if (e->builtin == 6) { int pad = push_cnt & 1; if (pad) emit("sub rsp, 8"); xsave(); emit("call ag__rt_yield"); xrestore(); if (pad) emit("add rsp, 8"); }
        else if (e->builtin == 5) { gen_expr(e->args[0]); if (has_drop(e->args[0]->ty->elem)) { emit("mov rdi, rax"); call_drop(e->args[0]->ty->elem); } }
        else if (e->builtin == 7) { gen_expr(e->args[0]); if (!is_agg(e->ty)) load(e->ty, "[rax]"); }   /* take(p): a raw move */
        else if (e->builtin == 8) { int L = newlbl(); gen_expr(e->args[0]); emit("test rax, rax"); emit("jz .L%d", L); emit("mov rax, QWORD PTR [rax-8]"); place(L); }   /* s.len() */
        else if (e->builtin == 9) { gen_expr(e->args[0]); emit("test rax, rax"); emit("lea rcx, [rip+.Lempty]"); emit("cmovz rax, rcx"); }   /* String for C: never null */
        else gen_call(e);
        break;
    case E_OK: case E_ERR: {   /* build tag + payload in the expression's temporary */
        Type *pt = e->k == E_ERR ? e->ty->err : e->ty->elem;
        if (e->a) {
            gen_expr(e->a);
            if (is_agg(pt)) { emit("mov rsi, rax"); emit("lea rdi, [rbp-%d]", e->tmp - 8); gen_copy(pt->size); }
            else store_local(pt, e->tmp - 8);
        }
        emit("mov QWORD PTR [rbp-%d], %d", e->tmp, e->k == E_ERR);
        emit("lea rax, [rbp-%d]", e->tmp); break;
    }
    case E_AWAIT: {
        Type *t = e->ty; int pad;
        gen_expr(e->a); emit("mov rdi, QWORD PTR [rax]"); emit("lea rsi, [rbp-%d]", e->tmp); emit("mov edx, %d", t->size);
        pad = push_cnt & 1; if (pad) emit("sub rsp, 8");
        xsave(); emit("call ag__rt_await"); xrestore(); if (pad) emit("add rsp, 8");
        if (is_agg(t)) emit("lea rax, [rbp-%d]", e->tmp);
        else if (t->k != TY_VOID) { char m[32]; snprintf(m, sizeof m, "[rbp-%d]", e->tmp); load(t, m); }
        break;
    }
    case E_TOIFACE:
        gen_expr(e->a); emit("mov QWORD PTR [rbp-%d], rax", e->tmp);
        emit("lea rax, [rip+.Lvt%lld]", e->ival); emit("mov QWORD PTR [rbp-%d], rax", e->tmp - 8);
        emit("lea rax, [rbp-%d]", e->tmp); break;
    case E_TRY: {   /* on Err: copy the error into our own return slot as Err(e) and return */
        Type *t = e->a->ty; int Lok = newlbl();
        gen_expr(e->a); emit("cmp QWORD PTR [rax], 0"); emit("je .L%d", Lok);
        emit("mov rdi, QWORD PTR [rbp-%d]", cur_fn->retslot); emit("mov QWORD PTR [rdi], 1");
        emit("lea rsi, [rax+8]"); emit("add rdi, 8"); emit("mov ecx, %d", t->err->size); emit("rep movsb");
        if (cur_gstmt) temps_drop(cur_gstmt);
        drops_from(0);
        emit("mov rax, QWORD PTR [rbp-%d]", cur_fn->retslot); emit("jmp .L%d", ret_lbl);
        place(Lok);
        if (t->elem->k != TY_VOID) { if (is_agg(t->elem)) emit("add rax, 8"); else load(t->elem, "[rax+8]"); }
        break;
    }
    case E_UN:
        switch (e->op) {
        case O_NEG: gen_expr(e->a); if (is_float(e->ty)) emit(e->ty->size == 4 ? "btc eax, 31" : "btc rax, 63"); else { emit("neg rax"); norm(e->ty); } break;
        case O_BNOT: gen_expr(e->a); emit("not rax"); norm(e->ty); break;
        case O_NOT: gen_expr(e->a); emit("xor eax, 1"); break;
        case O_DEREF:
            if (opt && !is_agg(e->ty)) { Addr a; char m[96]; if (lvaddr(e, &a, 1)) { addr_str(&a, m); load(e->ty, m); break; } }
            gen_expr(e->a); if (!is_agg(e->ty)) load(e->ty, "[rax]"); break;
        case O_ADDR: gen_addr(e->a); break;
        }
        break;
    case E_CAST: {
        Type *from = e->a->ty, *to = e->ty; gen_expr(e->a);
        if (is_float(from) && is_float(to)) {
            if (from->size != to->size) { to_xmm(0, from, "rax"); emit(to->size == 8 ? "cvtss2sd xmm0, xmm0" : "cvtsd2ss xmm0, xmm0"); from_xmm0(to); }
        } else if (is_float(to)) {   /* integer -> float (u64 above 2^63 handled by halving) */
            const char *cv = to->size == 8 ? "cvtsi2sd" : "cvtsi2ss";
            if (from->size == 8 && !from->sign) {
                int L1 = newlbl(), L2 = newlbl();
                emit("test rax, rax"); emit("js .L%d", L1); emit("%s xmm0, rax", cv); emit("jmp .L%d", L2);
                place(L1); emit("mov rcx, rax"); emit("shr rcx, 1"); emit("and eax, 1"); emit("or rcx, rax"); emit("%s xmm0, rcx", cv);
                emit(to->size == 8 ? "addsd xmm0, xmm0" : "addss xmm0, xmm0"); place(L2);
            } else emit("%s xmm0, rax", cv);
            from_xmm0(to);
        } else if (is_float(from)) {   /* float -> integer: truncates toward zero */
            to_xmm(0, from, "rax"); emit(from->size == 8 ? "cvttsd2si rax, xmm0" : "cvttss2si rax, xmm0"); norm(to);
        } else norm(to);
        break;
    }
    case E_BIN: {
        int op = e->op;
        if (op == O_LAND || op == O_LOR) {
            int L = newlbl(); gen_expr(e->a); emit("test rax, rax"); emit(op == O_LAND ? "jz .L%d" : "jnz .L%d", L);
            gen_expr(e->b); place(L); break;
        }
        if (opt && is_intlike(e->a->ty) && !(e->a->ty->k == TY_PTR && (op == O_ADD || op == O_SUB))) {
            Expr *L = e->a, *R = e->b; char rb[48];
            int comm = op == O_ADD || op == O_MUL || op == O_AND || op == O_OR || op == O_XOR || op == O_EQ || op == O_NE;
            if (comm && simple_int(L, rb) && !simple_int(R, rb) && !has_call(R)) { Expr *t2 = L; L = R; R = t2; }   /* constant/variable on the right */
            if (comm && !simple_int(R, rb)) {   /* keep L aside, evaluate R into rax, combine in either order */
                int t = 0; const char *sr = 0;
                if (!has_call(R) && scr_depth < 4) sr = SCR[scr_depth];
                else if ((t = tmp_gp())) sr = GREG[t];
                if (sr) {
                    gen_expr(L); emit("mov %s, rax", sr); if (!t) scr_depth++;
                    gen_expr(R); if (!t) scr_depth--; else gp_busy[t] = 0;
                    { long long lo, hi; nonorm = irange(e, &lo, &hi); } int_binop(op, e->a->ty, e->ty, sr, 0); break;
                }
            }
            gen_expr(L); gen_rhs(R, rb); { long long lo, hi; nonorm = irange(e, &lo, &hi); } int_binop(op, e->a->ty, e->ty, rb, R); break;
        }
        gen_expr(e->a); gpush("rax"); gen_expr(e->b); emit("mov rcx, rax"); gpop("rax");
        Type *t = e->a->ty;
        if (is_float(t)) {
            const char *sfx = t->size == 8 ? "sd" : "ss";
            to_xmm(0, t, "rax"); to_xmm(1, t, "rcx");
            switch (op) {
            case O_ADD: emit("add%s xmm0, xmm1", sfx); from_xmm0(t); break;
            case O_SUB: emit("sub%s xmm0, xmm1", sfx); from_xmm0(t); break;
            case O_MUL: emit("mul%s xmm0, xmm1", sfx); from_xmm0(t); break;
            case O_DIV: emit("div%s xmm0, xmm1", sfx); from_xmm0(t); break;
            default: {   /* comparisons are false when either side is NaN (except !=) */
                const char *uc = t->size == 8 ? "ucomisd" : "ucomiss";
                if (op == O_EQ) { emit("%s xmm0, xmm1", uc); emit("sete al"); emit("setnp cl"); emit("and al, cl"); }
                else if (op == O_NE) { emit("%s xmm0, xmm1", uc); emit("setne al"); emit("setp cl"); emit("or al, cl"); }
                else if (op == O_LT) { emit("%s xmm1, xmm0", uc); emit("seta al"); }
                else if (op == O_LE) { emit("%s xmm1, xmm0", uc); emit("setae al"); }
                else if (op == O_GT) { emit("%s xmm0, xmm1", uc); emit("seta al"); }
                else { emit("%s xmm0, xmm1", uc); emit("setae al"); }
                emit("movzx eax, al");
            }
            }
            break;
        }
        if (t->k == TY_PTR && (op == O_ADD || op == O_SUB)) { emit("imul rcx, rcx, %d", t->elem->size ? t->elem->size : 1); emit(op == O_ADD ? "add rax, rcx" : "sub rax, rcx"); break; }
        switch (op) {
        case O_ADD: emit("add rax, rcx"); norm(e->ty); break;
        case O_SUB: emit("sub rax, rcx"); norm(e->ty); break;
        case O_MUL: emit("imul rax, rcx"); norm(e->ty); break;
        case O_DIV: case O_MOD:
            if (t->sign) { emit("cqo"); emit("idiv rcx"); } else { emit("xor edx, edx"); emit("div rcx"); }
            if (op == O_MOD) emit("mov rax, rdx");
            norm(e->ty); break;
        case O_AND: emit("and rax, rcx"); break; case O_OR: emit("or rax, rcx"); break; case O_XOR: emit("xor rax, rcx"); break;
        case O_SHL: emit("shl rax, cl"); norm(e->ty); break;
        case O_SHR: emit(t->sign ? "sar rax, cl" : "shr rax, cl"); break;
        default: {
            int u = is_unsigned_cmp(t); const char *cc = "e";
            switch (op) {
            case O_EQ: cc = "e"; break; case O_NE: cc = "ne"; break;
            case O_LT: cc = u ? "b" : "l"; break; case O_LE: cc = u ? "be" : "le"; break;
            case O_GT: cc = u ? "a" : "g"; break; case O_GE: cc = u ? "ae" : "ge"; break;
            }
            emit("cmp rax, rcx"); emit("set%s al", cc); emit("movzx eax, al");
        }
        }
        break;
    }
    }
}
static void gen_copy(int size) { emit("mov ecx, %d", size); emit("rep movsb"); }

static void gen_stmt0(Stmt *s);
static const char *sized_reg(int r, int size) { return size == 1 ? GREG8[r] : size == 2 ? GREG16[r] : size == 4 ? GREG32[r] : GREG[r]; }
static const char *sized_rdx(int size) { return size == 1 ? "dl" : size == 2 ? "dx" : size == 4 ? "edx" : "rdx"; }
static const char *ptr_kw(int size) { return size == 1 ? "BYTE" : size == 2 ? "WORD" : size == 4 ? "DWORD" : "QWORD"; }
static int gen_fassign(Stmt *s) {   /* float assignments in xmm registers */
    Expr *x = s->e, *v = s->e2; int f32 = x->ty->size == 4; const char *mv = f32 ? "movss" : "movsd", *kw = f32 ? "DWORD" : "QWORD";
    int compound = v->k == E_BIN && v->a == x && v->op >= O_ADD && v->op <= O_DIV;
    Expr *val = compound ? v->b : v; char src[96];
    if (x->k == E_VAR && x->lv->reg >= 8) {
        int r = x->lv->reg;
        if (compound) {
            if (fsimple(val, 0)) { fsimple(val, src); fop(v->op, f32, r, src); }
            else { gen_fexpr(val, 0); fop(v->op, f32, r, "xmm0"); }
        } else if (fsimple(val, 0)) { fsimple(val, src); if (src[0] == 'x') emit("movaps xmm%d, %s", r, src); else emit("%s xmm%d, %s", mv, r, src); }
        else gen_fexpr_to(val, r);
        return 1;
    }
    if (x->k == E_VAR && x->lv->reg) return 0;
    Addr a; char m[96];
    if (!lv_cheap(x)) {   /* value first, then the address (index code leaves xmm0 alone) */
        if (!lv_anyok(x) || has_call(val) || can_fpe(val)) return 0;
        if (compound && fsimple(val, 0)) { fsimple(val, src); lvaddr_any(x, &a); addr_str(&a, m); emit("%s xmm1, %s PTR %s", mv, kw, m); fop(v->op, f32, 1, src); emit("%s %s PTR %s, xmm1", mv, kw, m); return 1; }
        gen_fexpr(val, 0); lvaddr_any(x, &a); addr_str(&a, m);
    } else { gen_fexpr(val, 0); lvaddr_must(x, &a); addr_str(&a, m); }
    if (compound) { emit("%s xmm1, %s PTR %s", mv, kw, m); fop(v->op, f32, 1, "xmm0"); emit("%s %s PTR %s, xmm1", mv, kw, m); }
    else emit("%s %s PTR %s, xmm0", mv, kw, m);
    return 1;
}
static int reads_greg(Expr *e, int r) {
    if (!e) return 0;
    if (e->k == E_VAR && e->lv && e->lv->reg == r) return 1;
    if (reads_greg(e->a, r) || reads_greg(e->b, r)) return 1;
    for (int i = 0; i < e->nargs; i++) if (reads_greg(e->args[i], r)) return 1;
    return 0;
}
/* integer expression straight into register variable r */
static void gen_int_to(Expr *e, int r) {
    char rb[48]; long long lo, hi;
    if (simple_int(e, rb)) { if (!strcmp(rb, "0")) emit("xor %s, %s", GREG32[r], GREG32[r]); else if (strcmp(rb, GREG[r])) emit("mov %s, %s", GREG[r], rb); return; }
    if (e->k == E_BIN && is_int(e->ty) && is_int(e->a->ty) && (e->op == O_ADD || e->op == O_SUB || e->op == O_AND || e->op == O_OR || e->op == O_XOR || e->op == O_MUL)) {
        Expr *L = e->a, *R = e->b; int op = e->op, nn = e->ty->size == 8 || irange(e, &lo, &hi);
        int comm = op != O_SUB;
        if (comm && simple_int(L, rb) && !simple_int(R, rb) && !has_call(R)) { Expr *t = L; L = R; R = t; }
        if (simple_int(R, rb) && !reads_greg(R, r)) {
            int lr = regvar(L), imm = R->k == E_INT && R->cls == 0 && imm32(R->ival);
            if (lr && imm && imm32(-R->ival) && (op == O_ADD || op == O_SUB) && nn) { long long d = op == O_ADD ? R->ival : -R->ival; emit("lea %s, [%s%+lld]", GREG[r], GREG[lr], d); return; }
            if (op == O_MUL && imm) { emit("imul %s, %s, %lld", GREG[r], lr ? GREG[lr] : (gen_int_to(L, r), GREG[r]), R->ival); if (!nn) norm_reg(e->ty, r); return; }
            static const char *mn[] = { "add", "sub", "imul", "", "", "and", "or", "xor" };
            gen_int_to(L, r); emit("%s %s, %s", mn[op], GREG[r], rb);
            if (!nn && op != O_AND && op != O_OR && op != O_XOR) norm_reg(e->ty, r);
            return;
        }
    }
    gen_expr(e); emit("mov %s, rax", GREG[r]);
}
/* ---- facts from loop conditions: inside 'while x < y' x cannot be the type's maximum, so x += 1 cannot wrap ---- */
typedef struct { Local *l; int lower; long long v; } Fact;
static Fact facts[64]; static int nfacts;
static int stmt_assigns(Stmt *s, Local *l) {
    if (!s) return 0;
    if ((s->k == S_ASSIGN && s->e->k == E_VAR && s->e->lv == l) || (s->k == S_LET && s->lv == l)) return 1;
    if (stmt_assigns(s->body, l) || stmt_assigns(s->els, l)) return 1;
    for (int i = 0; i < s->n; i++) if (stmt_assigns(s->list[i], l)) return 1;
    return 0;
}
static void facts_kill(Local *l) { for (int i = 0; i < nfacts; i++) if (facts[i].l == l) facts[i].l = 0; }
static void facts_kill_assigned(Stmt *s) { for (int i = 0; i < nfacts; i++) if (facts[i].l && stmt_assigns(s, facts[i].l)) facts[i].l = 0; }
static int ty_limits(Type *t, long long *mn, long long *mx) {
    if (t->k != TY_INT || (t->size == 8 && !t->sign)) return 0;
    if (t->size == 8) { *mn = (long long)(1ULL << 63); *mx = (long long)((1ULL << 63) - 1); return 1; }
    int b = t->size * 8;
    if (t->sign) { *mx = (1LL << (b - 1)) - 1; *mn = -*mx - 1; } else { *mn = 0; *mx = (1LL << b) - 1; }
    return 1;
}
static void fact_add(Local *l, int lower, long long v) { if (nfacts < 64) { facts[nfacts].l = l; facts[nfacts].lower = lower; facts[nfacts++].v = v; } }
static int fact_get(Local *l, int lower, long long *v) {
    int found = 0;
    for (int i = 0; i < nfacts; i++) if (facts[i].l == l && facts[i].lower == lower) {
        if (!found || (lower ? facts[i].v > *v : facts[i].v < *v)) *v = facts[i].v;
        found = 1;
    }
    return found;
}
static void facts_from(Expr *e) {   /* conditions known true at the start of a loop body */
    if (e->k == E_BIN && e->op == O_LAND) { facts_from(e->a); facts_from(e->b); return; }
    if (e->k != E_BIN || e->op < O_LT || e->op > O_GE || !e->a->ty || e->a->ty->k != TY_INT) return;
    Expr *A = e->a, *B = e->b; int strict = e->op == O_LT || e->op == O_GT;
    if (e->op == O_GT || e->op == O_GE) { A = e->b; B = e->a; }   /* now A < B or A <= B */
    long long mn, mx; if (!ty_limits(e->a->ty, &mn, &mx)) return;
    int ak = A->k == E_INT && A->cls == 0, bk = B->k == E_INT && B->cls == 0;
    if (regvar(A)) { if (bk) fact_add(A->lv, 0, strict ? B->ival - 1 : B->ival); else if (strict) fact_add(A->lv, 0, mx - 1); }
    if (regvar(B)) { if (ak) fact_add(B->lv, 1, strict ? A->ival + 1 : A->ival); else if (strict) fact_add(B->lv, 1, mn + 1); }
}
static int gen_assign_opt(Stmt *s) {   /* integer assignments without the push/pop dance; 0 = not handled */
    Expr *x = s->e, *v = s->e2; Type *t = x->ty; char rb[48];
    if (is_float(t)) return gen_fassign(s);
    if (!is_intlike(t)) return 0;
    int compound = v->k == E_BIN && v->a == x && v->op >= O_ADD && v->op <= O_SHR;
    int r = regvar(x);
    if (r) {
        if (compound && (v->op == O_ADD || v->op == O_SUB || v->op == O_AND || v->op == O_OR || v->op == O_XOR) && simple_int(v->b, rb)) {
            static const char *mn[] = { "add", "sub", "", "", "", "and", "or", "xor" };
            emit("%s %s, %s", mn[v->op], GREG[r], rb);
            if (v->op <= O_SUB) {
                long long lim, mn2, mx2, c = v->b->k == E_INT && v->b->cls == 0 ? v->b->ival : -1;
                int safe = c > 0 && c < (1LL << 32) && ty_limits(v->ty, &mn2, &mx2) &&
                           (v->op == O_ADD ? fact_get(x->lv, 0, &lim) && lim <= mx2 - c : fact_get(x->lv, 1, &lim) && lim >= mn2 + c);
                if (!safe) norm_reg(v->ty, r);
            }
            return 1;
        }
        if (simple_int(v, rb)) { if (!strcmp(rb, "0")) emit("xor %s, %s", GREG32[r], GREG32[r]); else emit("mov %s, %s", GREG[r], rb); return 1; }
        gen_int_to(v, r); return 1;
    }
    if (x->k == E_VAR && x->lv->reg) return 0;
    Addr a; char m[96]; int sz = t->size;
    if (compound && (v->op == O_ADD || v->op == O_SUB || v->op == O_AND || v->op == O_OR || v->op == O_XOR)) {   /* op [mem], value */
        static const char *mn[] = { "add", "sub", "", "", "", "and", "or", "xor" };
        int ir = regvar(v->b);
        if (v->b->k == E_INT && imm32(v->b->ival)) { if (!lvaddr(x, &a, 1)) return 0; addr_str(&a, m); emit("%s %s PTR %s, %lld", mn[v->op], ptr_kw(sz), m, v->b->ival); return 1; }
        if (ir) { if (!lvaddr(x, &a, 1)) return 0; addr_str(&a, m); emit("%s %s PTR %s, %s", mn[v->op], ptr_kw(sz), m, sized_reg(ir, sz)); return 1; }
        if (!lv_cheap(x)) return 0;
        gen_expr(v->b); emit("mov rdx, rax"); lvaddr_must(x, &a); addr_str(&a, m);
        emit("%s %s PTR %s, %s", mn[v->op], ptr_kw(sz), m, sized_rdx(sz)); return 1;
    }
    int ir = regvar(v);
    if ((v->k == E_INT || v->k == E_BOOL || v->k == E_ENUMVAL) && imm32(v->ival)) {
        if (!lvaddr(x, &a, 1)) return 0; addr_str(&a, m); emit("mov %s PTR %s, %lld", ptr_kw(sz), m, sz == 1 ? (v->ival & 255) : sz == 2 ? (v->ival & 65535) : v->ival); return 1;
    }
    if (ir) { if (!lvaddr(x, &a, 1)) return 0; addr_str(&a, m); emit("mov %s PTR %s, %s", ptr_kw(sz), m, sized_reg(ir, sz)); return 1; }
    if (!lv_cheap(x)) return 0;
    if (lv_nocode(x)) { gen_expr(v); lvaddr_must(x, &a); addr_str(&a, m); emit("mov %s PTR %s, %s", ptr_kw(sz), m, sz == 1 ? "al" : sz == 2 ? "ax" : sz == 4 ? "eax" : "rax"); return 1; }
    gen_expr(v); emit("mov rdx, rax"); lvaddr_must(x, &a); addr_str(&a, m); emit("mov %s PTR %s, %s", ptr_kw(sz), m, sized_rdx(sz)); return 1;
}
/* ---- full unrolling of short constant-trip loops: each copy sees the loop variable as a constant ---- */
static int unroll_bad;
static int ucount(Expr *e, Local *v) {   /* node count; flags shapes that cannot be cloned */
    if (!e) return 0;
    if (e->tmp || e->dflag || e->move || e->k == E_AWAIT || e->k == E_TRY || e->k == E_OK || e->k == E_ERR || e->k == E_TOIFACE) unroll_bad = 1;
    if ((e->k == E_REF || (e->k == E_UN && e->op == O_ADDR)) && e->a->k == E_VAR && e->a->lv == v) unroll_bad = 1;
    int n = 1 + ucount(e->a, v) + ucount(e->b, v);
    for (int i = 0; i < e->nargs; i++) n += ucount(e->args[i], v);
    return n;
}
static int scount(Stmt *s, Local *v) {
    if (!s) return 0;
    if (s->temps || s->k == S_MATCH || (s->k == S_LET && s->lv->dflag)) unroll_bad = 1;
    if (s->k == S_FOR && !s->is_range) unroll_bad = 1;
    int n = 1 + ucount(s->e, v) + ucount(s->e2, v) + scount(s->body, v) + scount(s->els, v);
    for (int i = 0; i < s->n; i++) n += scount(s->list[i], v);
    return n;
}
static Expr *uclone(Expr *e, Local *v, long long k) {
    if (!e) return 0;
    Expr *c = xalloc(sizeof *c); *c = *e; c->inl = 0; c->inl_done = 0;
    if (e->k == E_VAR && e->lv == v) { c->k = E_INT; c->ival = k; c->cls = 0; c->lv = 0; return c; }
    c->a = uclone(e->a, v, k); c->b = uclone(e->b, v, k);
    if (e->nargs) { c->args = xalloc(sizeof(Expr *) * (size_t)e->nargs); for (int i = 0; i < e->nargs; i++) c->args[i] = uclone(e->args[i], v, k); }
    return c;
}
static Stmt *sclone(Stmt *s, Local *v, long long k) {
    if (!s) return 0;
    Stmt *c = xalloc(sizeof *c); *c = *s;
    c->e = uclone(s->e, v, k);
    if (s->k == S_ASSIGN && s->e2->k == E_BIN && s->e2->a == s->e) {   /* compound assignment keeps its shared target node */
        c->e2 = xalloc(sizeof *c->e2); *c->e2 = *s->e2; c->e2->a = c->e; c->e2->b = uclone(s->e2->b, v, k);
    } else c->e2 = uclone(s->e2, v, k);
    fold(c->e); fold(c->e2);
    c->body = sclone(s->body, v, k); c->els = sclone(s->els, v, k);
    if (s->n) { c->list = xalloc(sizeof(Stmt *) * (size_t)s->n); for (int i = 0; i < s->n; i++) c->list[i] = sclone(s->list[i], v, k); }
    return c;
}
static void gen_stmt(Stmt *s);
static int gen_unrolled(Stmt *s, int Lb) {
    if (s->e->k != E_INT || s->e2->k != E_INT || s->e->cls || s->e2->cls) return 0;
    long long lo = s->e->ival, hi = s->e2->ival;
    if (hi - lo < 1 || hi - lo > 16) return 0;
    unroll_bad = 0; int n = scount(s->body, s->lv);
    if (unroll_bad || n * (hi - lo) > 900) return 0;
    for (long long k = lo; k < hi; k++) {
        Stmt *b = sclone(s->body, s->lv, k); int Lc = newlbl();
        loop_dl[loop_sp] = ndl; loop_brk[loop_sp] = Lb; loop_cont[loop_sp++] = Lc; gen_stmt(b); loop_sp--;
        place(Lc);
    }
    place(Lb);
    return 1;
}
/* ---- loop vectorization (SSE2, 2 x f64 or 4 x f32) ----
 * for v in s..e { A[v + inv] (op)= <elementwise float expression of B[v + inv'], loop-invariant scalars>; ... }
 * Every array element is touched at most once per iteration at the same index, so packed execution gives exactly the
 * scalar results. A runtime check proves all indices are in bounds first; otherwise (and for the tail) the ordinary
 * scalar loop runs and traps at the right iteration. */
typedef struct { Local *arr; Expr *inv; int reg; } VStream;
static VStream vst[3]; static int nvst, vbad, vw; static Expr *vinvx[4]; static int nvinv; static Local *vlv; static Type *vty;
static const char *VSREG[] = { "rsi", "rdi", "rdx" };
static int expr_eq(Expr *a, Expr *b) {
    if (!a || !b || a == b) return a == b;
    if (a->k == E_FLOAT && b->k == E_FLOAT) return same(a->ty, b->ty) && !strcmp(a->sval, b->sval);
    if (a->k != b->k || a->op != b->op || !a->ty || !b->ty || !same(a->ty, b->ty) || a->nargs || b->nargs) return 0;
    if (a->k == E_INT && (a->ival != b->ival || a->cls != b->cls)) return 0;
    if (a->k == E_VAR && a->lv != b->lv) return 0;
    if (a->k == E_FIELD && a->fld != b->fld) return 0;
    if (a->k != E_INT && a->k != E_VAR && a->k != E_BIN && a->k != E_UN && a->k != E_CAST && a->k != E_FIELD) return 0;
    return expr_eq(a->a, b->a) && expr_eq(a->b, b->b);
}
static int uses_lv(Expr *e, Local *v) {
    if (!e) return 0;
    if (e->k == E_VAR && e->lv == v) return 1;
    if (uses_lv(e->a, v) || uses_lv(e->b, v)) return 1;
    for (int i = 0; i < e->nargs; i++) if (uses_lv(e->args[i], v)) return 1;
    return 0;
}
static int vinv_int_ok(Expr *e) {   /* loop-invariant index offset: plain integer arithmetic, cannot trap */
    if (!e) return 1;
    switch (e->k) {
    case E_INT: return e->cls == 0;
    case E_VAR: return e->lv && !e->lv->is_ref && e->lv->ty->k == TY_INT;
    case E_CAST: return is_int(e->a->ty) && vinv_int_ok(e->a);
    case E_BIN: return (e->op == O_ADD || e->op == O_SUB || e->op == O_MUL || e->op == O_AND || e->op == O_OR || e->op == O_XOR || e->op == O_SHL)
                       && vinv_int_ok(e->a) && vinv_int_ok(e->b);
    default: return 0;
    }
}
static int vinv_f_ok(Expr *e) {   /* loop-invariant float operand: scalars and arithmetic only (no loads that could trap, no calls) */
    if (!e) return 1;
    switch (e->k) {
    case E_INT: case E_FLOAT: return 1;
    case E_VAR: return e->lv && (!e->lv->is_ref || !is_agg(e->lv->ty)) && is_scalar(e->lv->ty);
    case E_CAST: return vinv_int_ok(e->a) || (is_float(e->a->ty) && vinv_f_ok(e->a));
    case E_BIN: return e->op >= O_ADD && e->op <= O_DIV && vinv_f_ok(e->a) && vinv_f_ok(e->b);
    case E_UN: return e->op == O_NEG && vinv_f_ok(e->a);
    default: return 0;
    }
}
static int vstream_of(Expr *x) {   /* x = A[v + inv]: index of its stream (added if new), -1 if not a stream access */
    if (x->k != E_INDEX || x->a->k != E_VAR || x->a->ty->k != TY_ARRAY || !same(x->ty, vty) || x->a->ty->n > 2147483647LL) return -1;
    Expr *ix = x->b, *inv = 0;
    if (!ix->ty || ix->ty->size != 8) return -1;
    if (ix->k == E_VAR && ix->lv == vlv) inv = 0;
    else if (ix->k == E_BIN && ix->op == O_ADD && ix->a->k == E_VAR && ix->a->lv == vlv && !uses_lv(ix->b, vlv)) inv = ix->b;
    else if (ix->k == E_BIN && ix->op == O_ADD && ix->b->k == E_VAR && ix->b->lv == vlv && !uses_lv(ix->a, vlv)) inv = ix->a;
    else return -1;
    if (!vinv_int_ok(inv)) return -1;
    Local *arr = x->a->lv;
    for (int i = 0; i < nvst; i++) if (vst[i].arr == arr && expr_eq(vst[i].inv, inv)) return i;
    if (nvst >= 3) return -1;
    vst[nvst].arr = arr; vst[nvst].inv = inv; return nvst++;
}
static int vexpr_check(Expr *e, int k) {   /* vectorizable at register level k (0-3)? */
    if (k > 3) return 0;
    if (!uses_lv(e, vlv)) {
        if (!is_float(e->ty) || !vinv_f_ok(e)) return 0;
        for (int i = 0; i < nvinv; i++) if (expr_eq(vinvx[i], e)) return 1;
        if (nvinv >= 4) return 0;
        vinvx[nvinv++] = e; return 1;
    }
    if (e->k == E_INDEX) return vstream_of(e) >= 0;
    if (e->k == E_BIN && e->op >= O_ADD && e->op <= O_DIV && same(e->ty, vty)) return vexpr_check(e->a, k) && vexpr_check(e->b, k + 1);
    return 0;
}
static int vinv_idx(Expr *e) { for (int i = 0; i < nvinv; i++) if (expr_eq(vinvx[i], e)) return i; return -1; }
static void gen_vexpr(Expr *e, int k) {
    const char *sx = vw == 2 ? "pd" : "ps";
    int b = uses_lv(e, vlv) ? -1 : vinv_idx(e);
    if (b >= 0) { emit("movaps xmm%d, xmm%d", k, 7 - b); return; }
    if (e->k == E_INDEX) { int si = vstream_of(e); emit("movup%c xmm%d, XMMWORD PTR [%s+%s*%d]", sx[1], k, VSREG[si], GREG[vlv->reg], vty->size); return; }
    static const char *mn[] = { "add", "sub", "mul", "div" };
    Expr *L = e->a, *R = e->b;
    if ((e->op == O_ADD || e->op == O_MUL) && !uses_lv(L, vlv) && uses_lv(R, vlv)) { L = e->b; R = e->a; }   /* invariant on the right */
    gen_vexpr(L, k);
    int rb = uses_lv(R, vlv) ? -1 : vinv_idx(R);
    if (rb >= 0) emit("%s%s xmm%d, xmm%d", mn[e->op - O_ADD], sx, k, 7 - rb);
    else { gen_vexpr(R, k + 1); emit("%s%s xmm%d, xmm%d", mn[e->op - O_ADD], sx, k, k + 1); }
}
static Stmt *vstmts[8]; static int nvstmts;
static int vcollect(Stmt *b) {
    if (b->k == S_BLOCK) { for (int i = 0; i < b->n; i++) if (!vcollect(b->list[i])) return 0; return 1; }
    if (b->k != S_ASSIGN || b->temps || nvstmts >= 8) return 0;
    vstmts[nvstmts++] = b; return 1;
}
static int gen_vector_loop(Stmt *s, const char *bo, int Lt) {
    Local *v = s->lv;
    if (!v->reg || v->reg >= 8 || v->ty->size != 8 || !s->body) return 0;
    nvstmts = 0; nvst = 0; nvinv = 0; vlv = v; vty = 0;
    if (!vcollect(s->body) || !nvstmts) return 0;
    for (int i = 0; i < nvstmts; i++) {
        Stmt *st = vstmts[i]; Expr *x = st->e, *val = st->e2;
        if (!x->ty || !is_float(x->ty)) return 0;
        if (!vty) vty = x->ty; else if (!same(vty, x->ty)) return 0;
        if (vstream_of(x) < 0) return 0;
        int compound = val->k == E_BIN && val->a == x && val->op >= O_ADD && val->op <= O_DIV;
        if (!vexpr_check(compound ? val->b : val, 1)) return 0;
    }
    /* aliasing: arrays reached through references may overlap anything; distinct named arrays never do */
    int anyref = 0; for (int i = 0; i < nvst; i++) if (vst[i].arr->is_ref) anyref = 1;
    for (int i = 0; i < nvst; i++) for (int j = 0; j < nvst; j++)
        if (i != j && (vst[i].arr == vst[j].arr || anyref)) return 0;   /* two index forms into one (possibly the same) array */
    vw = 16 / vty->size;
    const char *sx = vw == 2 ? "pd" : "ps";
    int Lfail = Lt;
    /* streams: base pointers and the bounds proof */
    for (int i = 0; i < nvst; i++) {
        Local *arr = vst[i].arr; long long n = arr->ty->n; int esz = vty->size;
        if (vst[i].inv) gen_expr(vst[i].inv); else emit("xor eax, eax");
        emit("lea rcx, [rax+%s]", GREG[v->reg]);                       /* first index */
        emit("mov r11, rax"); emit("add r11, %s", bo); emit("sub r11, 1");  /* last index */
        emit("cmp rcx, %lld", n); emit("jae .L%d", Lfail);
        emit("cmp r11, %lld", n); emit("jae .L%d", Lfail);
        emit("cmp rcx, r11"); emit("ja .L%d", Lfail);
        if (i == 0) { emit("sub r11, rcx"); emit("cmp r11, %d", vw - 1); emit("jl .L%d", Lfail); }
        Addr a; const char *sv = lv_tmp; lv_tmp = "rcx"; int ok = lvaddr(&(Expr){ .k = E_VAR, .lv = arr, .ty = arr->ty }, &a, 0); lv_tmp = sv;
        if (!ok) { fprintf(stderr, "internal error: vector base\n"); exit(3); }
        if (!strncmp(a.base, "rip+", 4)) memmove(a.base, a.base + 4, strlen(a.base) - 3);
        char m[96]; strcpy(a.idx, "rax"); a.scale = esz; addr_str(&a, m);
        emit("lea %s, %s", VSREG[i], m);
    }
    for (int i = 0; i < nvinv; i++) {   /* broadcast loop-invariant scalars into xmm7, xmm6, ... */
        gen_fexpr(vinvx[i], 0);
        if (vw == 2) { emit("movaps xmm%d, xmm0", 7 - i); emit("unpcklpd xmm%d, xmm%d", 7 - i, 7 - i); }
        else { emit("movaps xmm%d, xmm0", 7 - i); emit("shufps xmm%d, xmm%d, 0", 7 - i, 7 - i); }
    }
    emit("mov r11, %s", bo); emit("sub r11, %d", vw - 1);
    int Lv = newlbl();
    fputs("\t.p2align 4,,10\n", out); place(Lv);
    for (int i = 0; i < nvstmts; i++) {
        Stmt *st = vstmts[i]; Expr *x = st->e, *val = st->e2; int si = vstream_of(x);
        char m[96]; snprintf(m, sizeof m, "XMMWORD PTR [%s+%s*%d]", VSREG[si], GREG[v->reg], vty->size);
        int compound = val->k == E_BIN && val->a == x && val->op >= O_ADD && val->op <= O_DIV;
        if (compound) {
            static const char *mn[] = { "add", "sub", "mul", "div" };
            gen_vexpr(val->b, 1); emit("movup%c xmm0, %s", sx[1], m); emit("%s%s xmm0, xmm1", mn[val->op - O_ADD], sx);
        } else gen_vexpr(val, 0);
        emit("movup%c %s, xmm0", sx[1], m);
    }
    emit("add %s, %d", GREG[v->reg], vw);
    emit("cmp %s, r11", GREG[v->reg]); emit(v->ty->sign ? "jl .L%d" : "jb .L%d", Lv);
    return 1;
}
/* for i in A..B { arr[i] = constant; } with a fixed-size array and constant in-bounds limits: a block fill */
static int gen_fill_loop(Stmt *s) {
    Stmt *b = s->body;
    while (b && b->k == S_BLOCK && b->n == 1) b = b->list[0];
    if (!b || b->k != S_ASSIGN || b->temps || s->e->k != E_INT || s->e2->k != E_INT) return 0;
    Expr *x = b->e, *v = b->e2;
    if (x->k != E_INDEX || x->a->ty->k != TY_ARRAY || x->b->k != E_VAR || x->b->lv != s->lv || !lv_static(x->a)) return 0;
    int fzero = is_float(x->ty) && ((v->k == E_INT && v->ival == 0) || (v->k == E_FLOAT && !strchr(v->sval, '-') && strtod(v->sval, 0) == 0.0));
    long long val = 0, pat = 0;
    if (fzero) val = 0;
    else if (!(v->k == E_INT || v->k == E_BOOL || v->k == E_ENUMVAL) || v->cls == 2 || !is_intlike(x->ty)) return 0;
    else val = v->ival;
    long long lo = s->e->ival, hi = s->e2->ival, n = x->a->ty->n; int sz = x->ty->size;
    if (lo < 0 || hi > n || hi - lo < 16) return 0;
    if (sz == 1) pat = val & 255; else if (val == 0) pat = 0; else if (sz == 8) pat = val; else return 0;
    Addr a; char m[96];
    if (!lvaddr(x->a, &a, 0)) return 0;
    a.disp += lo * sz; addr_str(&a, m);
    emit("lea rdi, %s", m);
    if (sz == 8 && pat) { emit("mov rax, %lld", pat); emit("mov rcx, %lld", hi - lo); emit("rep stosq"); }
    else { emit(pat ? "mov eax, %lld" : "xor eax, eax", pat); emit("mov rcx, %lld", (hi - lo) * sz); emit("rep stosb"); }
    return 1;
}
static void gen_for_opt(Stmt *s, int Lt, int Lc, int Lb) {
    if (gen_fill_loop(s) || gen_unrolled(s, Lb)) return;
    Local *v = s->lv, *bd = s->bnd; int Lbody = newlbl(); char ib[48];
    if (v->reg && v->reg < 8 && simple_int(s->e, ib)) { if (!strcmp(ib, "0")) emit("xor %s, %s", GREG32[v->reg], GREG32[v->reg]); else emit("mov %s, %s", GREG[v->reg], ib); }
    else { gen_expr(s->e); store_var(v); }
    int cimm = s->e2->k == E_INT && s->e2->ival >= -2147483647LL && s->e2->ival <= 2147483647LL, bvar = !cimm && for_bound_var(s);
    if (!cimm && !bvar) {
        if (bd->reg && bd->reg < 8 && simple_int(s->e2, ib)) emit("mov %s, %s", GREG[bd->reg], ib);
        else { gen_expr(s->e2); store_var(bd); }
    }
    v->has_range = 0;
    { long long lo, hi; if (cimm && irange(s->e, &lo, &hi) && lo <= s->e2->ival - 1) { v->has_range = 1; v->lo = lo; v->hi = s->e2->ival - 1; } }
    char bo[48];
    if (cimm) snprintf(bo, sizeof bo, "%lld", s->e2->ival);
    else if (bvar) { Local *b = s->e2->lv; if (b->reg) snprintf(bo, sizeof bo, "%s", GREG[b->reg]); else snprintf(bo, sizeof bo, "QWORD PTR [rbp-%d]", b->off); }
    else if (bd->reg) snprintf(bo, sizeof bo, "%s", GREG[bd->reg]);
    else snprintf(bo, sizeof bo, "QWORD PTR [rbp-%d]", bd->off);
    gen_vector_loop(s, bo, Lt);
    emit("jmp .L%d", Lt);
    fputs("\t.p2align 4,,10\n", out); place(Lbody);
    loop_dl[loop_sp] = ndl; loop_brk[loop_sp] = Lb; loop_cont[loop_sp++] = Lc; gen_stmt(s->body); loop_sp--;
    place(Lc);
    /* i < bound held before the increment, so i + 1 cannot overflow: no re-normalisation needed */
    if (v->reg) emit("add %s, 1", GREG[v->reg]); else { load_var(v); emit("add rax, 1"); store_var(v); }
    place(Lt);
    char vo[48];
    if (v->reg) snprintf(vo, sizeof vo, "%s", GREG[v->reg]); else { load_var(v); snprintf(vo, sizeof vo, "rax"); }
    emit("cmp %s, %s", vo, bo); emit(v->ty->sign ? "jl .L%d" : "jb .L%d", Lbody);
    place(Lb);
}
static void gen_stmt(Stmt *s) {
    Stmt *prev = cur_gstmt; cur_gstmt = s;
    if (s->k != S_WHILE) temps_zero(s);
    gen_stmt0(s);
    if (s->k == S_ASSIGN && s->e->k == E_VAR) facts_kill(s->e->lv);
    if (s->k == S_LET) facts_kill(s->lv);
    if (s->k == S_LET || s->k == S_ASSIGN || s->k == S_EXPR) temps_drop(s);
    cur_gstmt = prev;
}
static void gen_stmt0(Stmt *s) {
    switch (s->k) {
    case S_BLOCK: scope_open(); for (int i = 0; i < s->n; i++) gen_stmt(s->list[i]); scope_close(); break;
    case S_LET: {
        Local *l = s->lv;
        if (is_agg(l->ty)) {
            if (s->e) { gen_expr(s->e); emit("mov rsi, rax"); emit("lea rdi, [rbp-%d]", l->off); gen_copy(l->ty->size); }
            else { emit("lea rdi, [rbp-%d]", l->off); emit("xor eax, eax"); emit("mov ecx, %d", l->ty->size); emit("rep stosb"); }
        } else {
            char rb[48]; int r = opt && l->reg && l->reg < 8 ? l->reg : 0;
            if (r && (!s->e || simple_int(s->e, rb))) { if (!s->e || !strcmp(rb, "0")) emit("xor %s, %s", GREG32[r], GREG32[r]); else emit("mov %s, %s", GREG[r], rb); }
            else if (r && is_intlike(l->ty) && !l->is_ref) gen_int_to(s->e, r);
            else if (opt && l->reg >= 8) { if (s->e) gen_fexpr_to(s->e, l->reg); else emit("pxor xmm%d, xmm%d", l->reg, l->reg); }
            else if (opt && is_float(l->ty) && s->e) { gen_fexpr(s->e, 0); emit(l->ty->size == 4 ? "movss DWORD PTR [rbp-%d], xmm0" : "movsd QWORD PTR [rbp-%d], xmm0", l->off); }
            else { if (s->e) gen_expr(s->e); else emit("xor eax, eax"); store_var(l); }
            xlive(l);
        }
        if (l->dflag) reg_local(l);
        break;
    }
    case S_ASSIGN:
        if (has_drop(s->e->ty)) {   /* drop the old value (unless moved out / raw pointer write), then move in */
            Expr *x = s->e; int flagged = x->k == E_VAR && x->lv->dflag, raw = x->k == E_UN;
            gen_expr(s->e2); gpush("rax"); gen_addr(x); gpush("rax");
            if (!raw && !s->init_store) {
                int L = -1; if (flagged) { L = newlbl(); emit("cmp BYTE PTR [rbp-%d], 0", x->lv->dflag); emit("je .L%d", L); }
                emit("mov rdi, QWORD PTR [rsp]"); call_drop(x->ty); if (L >= 0) place(L);
            }
            if (is_agg(x->ty)) { gpop("rdi"); gpop("rsi"); gen_copy(x->ty->size); }
            else { gpop("rdi"); gpop("rax"); emit("mov QWORD PTR [rdi], rax"); }   /* String */
            if (flagged) emit("mov BYTE PTR [rbp-%d], 1", x->lv->dflag);
        } else if (is_agg(s->e->ty)) { gen_expr(s->e2); gpush("rax"); gen_addr(s->e); emit("mov rdi, rax"); gpop("rsi"); gen_copy(s->e->ty->size); }
        else if (opt && gen_assign_opt(s)) { }
        else if (s->e->k == E_VAR && s->e->lv->reg && !s->e->lv->is_ref) { gen_expr(s->e2); store_var(s->e->lv); }
        else { gen_expr(s->e2); gpush("rax"); gen_addr(s->e); gpop("rdx"); store_ind(s->e->ty); }
        break;
    case S_EXPR: gen_expr(s->e); break;
    case S_IF: {
        int Le = newlbl(), Ld = newlbl();
        if (opt && !s->temps) gen_branch(s->e, Le, 0);
        else { gen_expr(s->e); temps_drop_keep(s); emit("test rax, rax"); emit("jz .L%d", Le); }
        gen_stmt(s->body);
        if (s->els) { emit("jmp .L%d", Ld); place(Le); gen_stmt(s->els); place(Ld); } else place(Le);
        break;
    }
    case S_WHILE: {
        int Lc = newlbl(), Lb = newlbl();
        if (opt) facts_kill_assigned(s);
        if (opt && !s->temps) {   /* jump to the test, body, test at the bottom */
            int Ltop = newlbl(); emit("jmp .L%d", Lc); fputs("\t.p2align 4,,10\n", out); place(Ltop);
            int fmark = nfacts; facts_from(s->e);
            loop_dl[loop_sp] = ndl; loop_brk[loop_sp] = Lb; loop_cont[loop_sp++] = Lc; gen_stmt(s->body); loop_sp--;
            nfacts = fmark;
            place(Lc); gen_branch(s->e, Ltop, 1); place(Lb); break;
        }
        place(Lc); temps_zero(s); gen_expr(s->e); temps_drop_keep(s); emit("test rax, rax"); emit("jz .L%d", Lb);
        loop_dl[loop_sp] = ndl; loop_brk[loop_sp] = Lb; loop_cont[loop_sp++] = Lc; gen_stmt(s->body); loop_sp--; emit("jmp .L%d", Lc); place(Lb);
        break;
    }
    case S_FOR: {
        int Lt = newlbl(), Lc = newlbl(), Lb = newlbl(); Local *v = s->lv; char m[32];
        if (opt) facts_kill_assigned(s);
        if (s->is_range && opt) { scope_open(); gen_for_opt(s, Lt, Lc, Lb); scope_close(); break; }
        if (opt) scope_open();
        if (s->is_range) {
            gen_expr(s->e); store_local(v->ty, v->off); gen_expr(s->e2); emit("mov QWORD PTR [rbp-%d], rax", s->slot1);
            place(Lt); snprintf(m, sizeof m, "[rbp-%d]", v->off); load(v->ty, m);
            emit("cmp rax, QWORD PTR [rbp-%d]", s->slot1); emit(v->ty->sign ? "jge .L%d" : "jae .L%d", Lb);
            loop_dl[loop_sp] = ndl; loop_brk[loop_sp] = Lb; loop_cont[loop_sp++] = Lc; gen_stmt(s->body); loop_sp--;
            place(Lc); load(v->ty, m); emit("add rax, 1"); norm(v->ty); store_local(v->ty, v->off); emit("jmp .L%d", Lt);
        } else {
            Type *at = s->e->ty;
            emit("mov QWORD PTR [rbp-%d], 0", s->slot1);
            place(Lt); emit("mov rax, QWORD PTR [rbp-%d]", s->slot1); emit("cmp rax, %lld", at->n); emit("jae .L%d", Lb);
            gen_expr(s->e); emit("mov rcx, QWORD PTR [rbp-%d]", s->slot1);
            if (at->elem->size != 1) emit("imul rcx, rcx, %d", at->elem->size);
            emit("add rax, rcx"); load(at->elem, "[rax]"); store_var(v); xlive(v);
            loop_dl[loop_sp] = ndl; loop_brk[loop_sp] = Lb; loop_cont[loop_sp++] = Lc; gen_stmt(s->body); loop_sp--;
            place(Lc); emit("add QWORD PTR [rbp-%d], 1", s->slot1); emit("jmp .L%d", Lt);
        }
        place(Lb); if (opt) scope_close(); break;
    }
    case S_RET:
        if (s->e) {
            gen_expr(s->e);
            if (is_agg(cur_fn->ret)) { emit("mov rsi, rax"); emit("mov rdi, QWORD PTR [rbp-%d]", cur_fn->retslot); gen_copy(cur_fn->ret->size); emit("mov rax, QWORD PTR [rbp-%d]", cur_fn->retslot); }
        }
        if (s->temps || ndl) { gpush("rax"); gpush("rax"); temps_drop(s); drops_from(0); gpop("rax"); gpop("rax"); }
        emit("jmp .L%d", ret_lbl); break;
    case S_BRK: drops_from(loop_dl[loop_sp - 1]); emit("jmp .L%d", loop_brk[loop_sp - 1]); break;
    case S_CONT: drops_from(loop_dl[loop_sp - 1]); emit("jmp .L%d", loop_cont[loop_sp - 1]); break;
    case S_MATCH: {
        int Lend = newlbl(); gen_expr(s->e); emit("mov QWORD PTR [rbp-%d], rax", s->slot1); temps_drop(s);
        if (s->e->ty->k == TY_DATA) {   /* compare the tag, then move the values into the bindings ('_' drops its value) */
            EnumDef *ed = s->e->ty->ed;
            for (int i = 0; i < s->n; i++) {
                int Ln = newlbl(); Expr *p = s->pats[i];
                scope_open();
                if (p) {
                    VarInfo *v = &ed->vi[p->ival];
                    emit("mov rax, QWORD PTR [rbp-%d]", s->slot1); emit("cmp QWORD PTR [rax], %lld", p->ival); emit("jne .L%d", Ln);
                    for (int j = 0; j < v->nf; j++) {
                        Type *ft = v->ft[j]; Local *b = s->vlv[i][j];
                        if (j) emit("mov rax, QWORD PTR [rbp-%d]", s->slot1);
                        if (b) {
                            if (is_agg(ft)) { emit("lea rsi, [rax+%d]", v->off[j]); emit("lea rdi, [rbp-%d]", b->off); gen_copy(ft->size); }
                            else { char src[32]; snprintf(src, sizeof src, "[rax+%d]", v->off[j]); load(ft, src); store_var(b); xlive(b); }
                            if (b->dflag) reg_local(b);
                        } else if (has_drop(ft)) { emit("lea rdi, [rax+%d]", v->off[j]); call_drop(ft); }
                    }
                } else if (has_drop(s->e->ty)) { emit("mov rdi, QWORD PTR [rbp-%d]", s->slot1); call_drop(s->e->ty); }   /* '_' arm */
                gen_stmt(s->list[i]); scope_close(); emit("jmp .L%d", Lend); place(Ln);
            }
            place(Lend); break;
        }
        if (s->e->ty->k == TY_RESULT) {
            for (int i = 0; i < s->n; i++) {
                int Ln = newlbl(); Expr *p = s->pats[i];
                scope_open();
                if (p) {
                    int er = p->name[0] == 'E' || p->name[0] == 'N'; Type *pt = er ? s->e->ty->err : s->e->ty->elem;   /* Err / None: tag 1 */
                    emit("mov rax, QWORD PTR [rbp-%d]", s->slot1); emit("cmp QWORD PTR [rax], %d", er); emit("jne .L%d", Ln);
                    Local *b = s->alv[i];
                    if (b) {
                        if (is_agg(pt)) { emit("lea rsi, [rax+8]"); emit("lea rdi, [rbp-%d]", b->off); gen_copy(pt->size); }
                        else { load(pt, "[rax+8]"); store_var(b); xlive(b); }
                        if (b->dflag) reg_local(b);
                    } else if (has_drop(pt)) { emit("lea rdi, [rax+8]"); call_drop(pt); }   /* Ok(_) / Err(_): the payload is dropped */
                } else if (has_drop(s->e->ty)) { emit("mov rdi, QWORD PTR [rbp-%d]", s->slot1); call_drop(s->e->ty); }   /* '_' arm */
                gen_stmt(s->list[i]); scope_close(); emit("jmp .L%d", Lend); place(Ln);
            }
            place(Lend); break;
        }
        for (int i = 0; i < s->n; i++) {
            int Ln = newlbl();
            if (s->pats[i]) {
                emit("mov rax, QWORD PTR [rbp-%d]", s->slot1); emit("mov rcx, %lld", s->pats[i]->ival); emit("cmp rax, rcx"); emit("jne .L%d", Ln);
            }
            gen_stmt(s->list[i]); emit("jmp .L%d", Lend); place(Ln);
        }
        place(Lend); break;
    }
    }
}
/* peephole pass over a function body (text, one instruction per line) */
/* registers an instruction may write (by text): first operand, plus implicit ones */
static int reg_family(const char *op) {   /* 0-15 for general registers, -1 otherwise */
    static const char *f[16][4] = { {"rax","eax","ax","al"}, {"rbx","ebx","bx","bl"}, {"rcx","ecx","cx","cl"}, {"rdx","edx","dx","dl"},
        {"rsi","esi","si","sil"}, {"rdi","edi","di","dil"}, {"rbp","ebp","bp","bpl"}, {"rsp","esp","sp","spl"},
        {"r8","r8d","r8w","r8b"}, {"r9","r9d","r9w","r9b"}, {"r10","r10d","r10w","r10b"}, {"r11","r11d","r11w","r11b"},
        {"r12","r12d","r12w","r12b"}, {"r13","r13d","r13w","r13b"}, {"r14","r14d","r14w","r14b"}, {"r15","r15d","r15w","r15b"} };
    for (int i = 0; i < 16; i++) for (int j = 0; j < 4; j++) if (!strcmp(op, f[i][j])) return i;
    return -1;
}
static int chk_reg[16]; static long long chk_n[16];   /* bounds checks known to hold on the fall-through path */
static void chk_clobber(const char *l) {
    char mn[16], op[24]; int n = 0;
    if (sscanf(l, "\t%15s %n", mn, &n) < 1) return;
    if (!strcmp(mn, "cmp") || !strcmp(mn, "test") || !strcmp(mn, "ucomisd") || !strcmp(mn, "ucomiss") || mn[0] == 'j' || !strcmp(mn, "push")) return;
    if (!strcmp(mn, "call") || !strncmp(mn, "rep", 3) || !strcmp(mn, "syscall") || !strcmp(mn, "cqo") || !strcmp(mn, "idiv") || !strcmp(mn, "div")
        || !strcmp(mn, "mul") || !strcmp(mn, "xchg") || !strcmp(mn, "leave") || !strcmp(mn, "cpuid")) { memset(chk_reg, 0, sizeof chk_reg); return; }
    int k = 0; const char *p = l + n;
    while (*p && *p != ',' && *p != ' ' && k < 23) op[k++] = *p++;
    op[k] = 0;
    int r = reg_family(op);
    if (r >= 0) chk_reg[r] = 0;
    else if (!strncmp(op, "QWORD", 5) || !strncmp(op, "DWORD", 5) || !strncmp(op, "WORD", 4) || !strncmp(op, "BYTE", 4) || !strncmp(op, "XMMWORD", 7)) return;   /* memory destination */
    else if (strncmp(op, "xmm", 3)) memset(chk_reg, 0, sizeof chk_reg);   /* unknown: forget everything */
}
static void peephole(char *buf, int retl, const char *epi) {
    char rj[32]; snprintf(rj, sizeof rj, "\tjmp .L%d", retl);
    char **ln = 0; int n = 0, cap = 0;
    for (char *p = buf; *p; ) { char *q = strchr(p, '\n'); if (!q) q = p + strlen(p); if (n >= cap) { cap = cap ? cap * 2 : 256; ln = realloc(ln, (size_t)cap * sizeof *ln); } ln[n++] = p; if (*q) *q++ = 0; p = q; }
    int dead = 0;
    for (int i = 0; i < n; i++) {
        char *l = ln[i];
        if (l[0] != '\t') { dead = 0; memset(chk_reg, 0, sizeof chk_reg); fprintf(out, "%s\n", l); continue; }   /* label */
        if (l[1] == '.') { fprintf(out, "%s\n", l); continue; }   /* directive */
        if (dead) continue;   /* unreachable after an unconditional jump */
        {   /* cmp REG, N / jae .Ltrap already established on this path: drop the repeat */
            char rg[8]; long long nn; int r;
            if (sscanf(l, "\tcmp %7[a-z0-9], %lld", rg, &nn) == 2 && i + 1 < n && !strcmp(ln[i + 1], "\tjae .Ltrap") && (r = reg_family(rg)) >= 0 && !strcmp(rg, (const char *[]){"rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","r8","r9","r10","r11","r12","r13","r14","r15"}[r])) {
                if (chk_reg[r] && chk_n[r] <= nn) { i++; continue; }
                chk_reg[r] = 1; chk_n[r] = nn;
                fprintf(out, "%s\n%s\n", l, ln[i + 1]); i++; continue;
            }
        }
        chk_clobber(l);
        if (!strcmp(l, "\tpush rax") && i + 1 < n && !strncmp(ln[i + 1], "\tpop ", 5)) {   /* push rax / pop X -> mov X, rax */
            if (strcmp(ln[i + 1] + 5, "rax")) fprintf(out, "\tmov %s, rax\n", ln[i + 1] + 5);
            i++; continue;
        }
        if (!strncmp(l, "\tjmp .L", 7) && i + 1 < n) {   /* jump to one of the next labels (skipping dead code) */
            size_t k = strlen(l + 5); int j = i + 1, hit = 0;
            while (j < n && ln[j][0] == '\t') j++;
            for (; j < n && ln[j][0] != '\t'; j++) if (!strncmp(ln[j], l + 5, k) && ln[j][k] == ':' && !ln[j][k + 1]) hit = 1;
            if (hit) { dead = 1; continue; }
        }
        if (epi && !strcmp(l, rj)) { fputs(epi, out); dead = 1; continue; }   /* return: duplicate the short epilogue */
        fprintf(out, "%s\n", l);
        if (!strncmp(l, "\tjmp ", 5) || !strcmp(l, "\tret")) dead = 1;
    }
    free(ln);
}
static void gen_func(Func *f) {
    if (f->is_extern || f->ifc) return;
    push_cnt = 0; ret_lbl = newlbl(); loop_sp = 0; cur_fn = f; ndl = 0; ndscope = 0; nxl = 0; nfacts = 0;
    if (opt) opt_analyze(f); else { nolocs = 0; f_has_xmm = 0; f->nsaved = 0; }
    FILE *fout = out; char *fbuf = 0; size_t flen = 0;
    if (opt) out = open_memstream(&fbuf, &flen);   /* body first: the prologue depends on the registers it used */
    else {
        int frame = f->frame;
        fprintf(out, "ag_%s:\n", mangle(f->name));
        emit("push rbp"); emit("mov rbp, rsp"); if (frame) emit("sub rsp, %d", frame);
    }
    int k = is_agg(f->ret) ? 1 : 0;
    if (k) emit("mov QWORD PTR [rbp-%d], rdi", f->retslot);
    for (int i = 0; i < f->nparams; i++) {
        Local *l = f->plocal[i]; int agg = !l->is_ref && is_agg(l->ty), ri = i + k;
        int sz = (l->is_ref || agg) ? 8 : l->ty->size, off = agg ? l->tmp : l->off;
        if (l->reg) {   /* promoted parameter: straight from the argument register */
            if (ri < 6) { if (l->reg >= 8) emit(sz == 4 ? "movd xmm%d, %s" : "movq xmm%d, %s", l->reg, sz == 4 ? R32[ri] : R64[ri]); else emit("mov %s, %s", GREG[l->reg], R64[ri]); }
            else { emit("mov rax, QWORD PTR [rbp+%d]", 16 + 8 * (ri - 6)); store_var(l); }
            continue;
        }
        if (ri < 6) {
            switch (sz) {
            case 1: emit("mov BYTE PTR [rbp-%d], %s", off, R8[ri]); break; case 2: emit("mov WORD PTR [rbp-%d], %s", off, R16[ri]); break;
            case 4: emit("mov DWORD PTR [rbp-%d], %s", off, R32[ri]); break; default: emit("mov QWORD PTR [rbp-%d], %s", off, R64[ri]);
            }
        } else {
            emit("mov rax, QWORD PTR [rbp+%d]", 16 + 8 * (ri - 6));
            switch (sz) {
            case 1: emit("mov BYTE PTR [rbp-%d], al", off); break; case 2: emit("mov WORD PTR [rbp-%d], ax", off); break;
            case 4: emit("mov DWORD PTR [rbp-%d], eax", off); break; default: emit("mov QWORD PTR [rbp-%d], rax", off);
            }
        }
    }
    for (int i = 0; i < f->nparams; i++) {
        Local *l = f->plocal[i];
        if (!l->is_ref && is_agg(l->ty)) { emit("mov rsi, QWORD PTR [rbp-%d]", l->tmp); emit("lea rdi, [rbp-%d]", l->off); gen_copy(l->ty->size); }
    }
    scope_open();
    for (int i = 0; i < f->nparams; i++) { if (f->plocal[i]->dflag) reg_local(f->plocal[i]); xlive(f->plocal[i]); }   /* by-value parameters are owned by the callee */
    if (f->is_ctor) for (int i = 0; i < f->cls->nf; i++) {   /* field default values run before the constructor body */
        Field *fl = &f->cls->f[i]; if (!fl->init) continue;
        gen_expr(fl->init);
        if (is_agg(fl->ty)) { emit("mov rsi, rax"); if (f->plocal[0]->reg) emit("mov rdi, %s", GREG[f->plocal[0]->reg]); else emit("mov rdi, QWORD PTR [rbp-%d]", f->plocal[0]->off); if (fl->off) emit("add rdi, %d", fl->off); gen_copy(fl->ty->size); }
        else { gpush("rax"); load_var(f->plocal[0]); if (fl->off) emit("add rax, %d", fl->off); gpop("rdx"); store_ind(fl->ty); }
    }
    gen_stmt(f->body); scope_close(); emit("xor eax, eax"); place(ret_lbl);
    if (opt) {
        fclose(out); out = fout; f->nsaved = 0;
        for (int r = 1; r <= 5; r++) if (gp_used[r]) f->saved[f->nsaved++] = (char)r;
        int frame = f->nsaved ? (f->frame + 8 * f->nsaved + 15) / 16 * 16 : f->frame;
        /* no stack frame needed when the body never touches the stack: save registers with push/pop */
        int frameless = !f->is_async && !strstr(fbuf, "rbp") && !strstr(fbuf, "rsp") && !strstr(fbuf, "\tpush") && !strstr(fbuf, "\tpop");
        char epi[256]; int ep = 0;
        fprintf(out, "ag_%s:\n", mangle(f->name));
        if (frameless) {
            int pad = !(f->nsaved & 1);
            for (int i = 0; i < f->nsaved; i++) emit("push %s", GREG[(int)f->saved[i]]);
            if (pad) emit("sub rsp, 8");
            if (pad) ep += sprintf(epi + ep, "\tadd rsp, 8\n");
            for (int i = f->nsaved - 1; i >= 0; i--) ep += sprintf(epi + ep, "\tpop %s\n", GREG[(int)f->saved[i]]);
            sprintf(epi + ep, "\tret\n");
        } else {
            emit("push rbp"); emit("mov rbp, rsp"); if (frame) emit("sub rsp, %d", frame);
            for (int i = 0; i < f->nsaved; i++) emit("mov QWORD PTR [rbp-%d], %s", f->frame + 8 * (i + 1), GREG[(int)f->saved[i]]);
            for (int i = 0; i < f->nsaved; i++) ep += sprintf(epi + ep, "\tmov %s, QWORD PTR [rbp-%d]\n", GREG[(int)f->saved[i]], f->frame + 8 * (i + 1));
            sprintf(epi + ep, "\tleave\n\tret\n");
        }
        peephole(fbuf, ret_lbl, f->nsaved <= 3 ? epi : 0); free(fbuf);
        fputs(epi, out);
    } else { emit("leave"); emit("ret"); }
    if (f->is_async) {   /* spawn stub: same arguments as the function, starts it as a task, returns the task */
        fprintf(out, "ag__spawn_%s:\n", mangle(f->name));
        emit("push rbp"); emit("mov rbp, rsp"); emit("sub rsp, 48");
        for (int i = 0; i < 6; i++) emit("mov QWORD PTR [rsp+%d], %s", 8 * i, R64[i]);
        emit("lea rdi, [rip+ag_%s]", mangle(f->name)); emit("mov rsi, rsp");
        emit("mov edx, %d", f->ret->k == TY_VOID ? 0 : f->ret->size); emit("mov ecx, %d", is_agg(f->ret));
        emit("call ag__rt_spawn"); emit("leave"); emit("ret");
    }
}
static char *rt_async_text;   /* lib/rt/async.s, appended when the program uses tasks */
static char *rt_fault_text;   /* lib/rt/fault.s: runtime error messages, appended to every program */
static void gen_program(FILE *o) {
    out = o;
    fprintf(out, "# generated by svalinnc " SVALINN_VERSION " - module %s\n\t.intel_syntax noprefix\n\t.text\n", mod_name);
    for (int i = 0; i < nfuncs; i++) gen_func(funcs[i]);
    Func *m = 0; for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i]->name, "main")) m = funcs[i];
    fprintf(out, "\t.globl main\nmain:\n");
    emit("push rbp"); emit("mov rbp, rsp"); emit("call ag__rt_init");
    if (protect_process) {   /* PR_SET_DUMPABLE = 4, arg 0: blocks ptrace attach and /proc/pid/mem access by other processes */
        emit("push rdi"); emit("push rsi");
        emit("mov edi, 4"); emit("xor esi, esi"); emit("xor edx, edx"); emit("xor ecx, ecx"); emit("xor r8d, r8d"); emit("xor eax, eax"); emit("call prctl");
        emit("pop rsi"); emit("pop rdi");
    }
    if (test_mode) emit("call ag___svalinn_tests");   /* --test: runs the @test functions */
    else if (m->nparams == 1) emit("call ag___svalinn_main");   /* builds the Vec<String> of arguments, then calls main */
    else { emit("call ag_main"); if (m->ret->k == TY_VOID) emit("xor eax, eax"); }
    emit("pop rbp"); emit("ret");
    fprintf(out, ".Ltrap:\n\tjmp ag__rt_trap\n");
    fputs(rt_fault_text, out);
    if (uses_async) fputs(rt_async_text, out);
    for (int gi = 0; gi < nglue; gi++) {   /* drop functions: destructor(s), then fields / elements / payload */
        Type *t = glue_ty[gi]; push_cnt = 0;
        fprintf(out, "ag__drop_%s:\n", mangle(tname(t)));
        emit("push rbp"); emit("mov rbp, rsp"); emit("push rbx"); emit("push r12"); emit("mov rbx, rdi");
        if (t->k == TY_STRUCT) {
            for (StructDef *c = t->sd; c; c = c->base) if (c->dtor) { emit("mov rdi, rbx"); emit("call ag_%s", mangle(c->dtor->name)); }
            for (int i = t->sd->nf - 1; i >= 0; i--) { Field *fl = &t->sd->f[i]; if (!has_drop(fl->ty)) continue; emit("lea rdi, [rbx+%d]", fl->off); call_drop(fl->ty); }
        } else if (t->k == TY_ARRAY) {
            int L1 = newlbl(), L2 = newlbl();
            emit("xor r12d, r12d"); place(L1); emit("cmp r12, %lld", t->n); emit("jae .L%d", L2);
            emit("mov rdi, r12"); emit("imul rdi, rdi, %d", t->elem->size); emit("add rdi, rbx"); call_drop(t->elem);
            emit("add r12, 1"); emit("jmp .L%d", L1); place(L2);
        } else if (t->k == TY_TASK) {   /* dropping a task waits for it, then destroys its result */
            int Ld = newlbl();
            emit("mov r12, QWORD PTR [rbx]"); emit("test r12, r12"); emit("je .L%d", Ld);
            emit("mov rdi, r12"); emit("call ag__rt_join");
            if (has_drop(t->elem)) { emit("lea rdi, [r12+112]"); call_drop(t->elem); }
            emit("mov rdi, r12"); emit("call ag__rt_free"); place(Ld);
        } else if (t->k == TY_STR) {   /* release: free at the last owner (null and literals never get here) */
            int Ld = newlbl();
            emit("mov rax, QWORD PTR [rbx]"); emit("sub QWORD PTR [rax-16], 1"); emit("jnz .L%d", Ld);
            emit("lea rdi, [rax-16]"); emit("call free"); place(Ld);
        } else if (t->k == TY_RESULT) {
            int Le = newlbl(), Lx = newlbl();
            emit("cmp QWORD PTR [rbx], 0"); emit("jne .L%d", Le);
            if (has_drop(t->elem)) { emit("lea rdi, [rbx+8]"); call_drop(t->elem); }
            emit("jmp .L%d", Lx); place(Le);
            if (has_drop(t->err)) { emit("lea rdi, [rbx+8]"); call_drop(t->err); }
            place(Lx);
        } else if (t->k == TY_DATA) {   /* the values of the variant in the tag */
            int Lx = newlbl();
            for (int v = 0; v < t->ed->nvi; v++) {
                VarInfo *vi = &t->ed->vi[v]; int any = 0;
                for (int j = 0; j < vi->nf; j++) if (has_drop(vi->ft[j])) any = 1;
                if (!any) continue;
                int Ln = newlbl();
                emit("cmp QWORD PTR [rbx], %d", v); emit("jne .L%d", Ln);
                for (int j = vi->nf - 1; j >= 0; j--) if (has_drop(vi->ft[j])) { emit("lea rdi, [rbx+%d]", vi->off[j]); call_drop(vi->ft[j]); }
                emit("jmp .L%d", Lx); place(Ln);
            }
            place(Lx);
        }
        emit("pop r12"); emit("pop rbx"); emit("pop rbp"); emit("ret");
    }
    for (int gi = 0; gi < ncopy; gi++) {   /* copy functions: one more owner for every string inside */
        Type *t = copy_ty[gi]; push_cnt = 0;
        fprintf(out, "ag__copy_%s:\n", mangle(tname(t)));
        emit("push rbp"); emit("mov rbp, rsp"); emit("push rbx"); emit("push r12"); emit("mov rbx, rdi");
        if (t->k == TY_STRUCT) {
            for (int i = 0; i < t->sd->nf; i++) {
                Field *fl = &t->sd->f[i]; if (!has_drop(fl->ty)) continue;
                if (fl->ty->k == TY_STR) { emit("mov rax, QWORD PTR [rbx+%d]", fl->off); str_retain("rax"); }
                else { emit("lea rdi, [rbx+%d]", fl->off); call_copy(fl->ty); }
            }
        } else if (t->k == TY_ARRAY) {
            int L1 = newlbl(), L2 = newlbl();
            emit("xor r12d, r12d"); place(L1); emit("cmp r12, %lld", t->n); emit("jae .L%d", L2);
            emit("mov rdi, r12"); emit("imul rdi, rdi, %d", t->elem->size); emit("add rdi, rbx");
            if (t->elem->k == TY_STR) { emit("mov rax, QWORD PTR [rdi]"); str_retain("rax"); } else call_copy(t->elem);
            emit("add r12, 1"); emit("jmp .L%d", L1); place(L2);
        } else if (t->k == TY_RESULT) {
            int Le = newlbl(), Lx = newlbl();
            emit("cmp QWORD PTR [rbx], 0"); emit("jne .L%d", Le);
            if (has_drop(t->elem)) { if (t->elem->k == TY_STR) { emit("mov rax, QWORD PTR [rbx+8]"); str_retain("rax"); } else { emit("lea rdi, [rbx+8]"); call_copy(t->elem); } }
            emit("jmp .L%d", Lx); place(Le);
            if (has_drop(t->err)) { if (t->err->k == TY_STR) { emit("mov rax, QWORD PTR [rbx+8]"); str_retain("rax"); } else { emit("lea rdi, [rbx+8]"); call_copy(t->err); } }
            place(Lx);
        } else if (t->k == TY_DATA) {
            int Lx = newlbl();
            for (int v = 0; v < t->ed->nvi; v++) {
                VarInfo *vi = &t->ed->vi[v]; int any = 0;
                for (int j = 0; j < vi->nf; j++) if (has_drop(vi->ft[j])) any = 1;
                if (!any) continue;
                int Ln = newlbl();
                emit("cmp QWORD PTR [rbx], %d", v); emit("jne .L%d", Ln);
                for (int j = 0; j < vi->nf; j++) {
                    if (!has_drop(vi->ft[j])) continue;
                    if (vi->ft[j]->k == TY_STR) { emit("mov rax, QWORD PTR [rbx+%d]", vi->off[j]); str_retain("rax"); }
                    else { emit("lea rdi, [rbx+%d]", vi->off[j]); call_copy(vi->ft[j]); }
                }
                emit("jmp .L%d", Lx); place(Ln);
            }
            place(Lx);
        }
        emit("pop r12"); emit("pop rbx"); emit("pop rbp"); emit("ret");
    }
    fprintf(out, "\t.data\n");
    for (int i = 0; i < nglobals; i++) {
        Local *g = globals[i]; Expr *x = g->init; int sz = g->ty->size;
        if (g->is_const) continue;   /* constants have no storage */
        if (!x) { fprintf(out, "\t.bss\n\t.balign %d\nag_g_%s:\n\t.zero %d\n\t.data\n", g->ty->align, g->name, sz); continue; }   /* zero-initialised: no space in the binary */
        fprintf(out, "\t.balign %d\nag_g_%s:\n", g->ty->align, g->name);
        if (x->k == E_STR) fprintf(out, "\t.quad .LS%d\n", add_str(x->sval, x->slen));
        else if (x->k == E_FLOAT) emit_fbits(x->sval, sz == 4);
        else if (is_float(g->ty)) { char tx[32]; snprintf(tx, sizeof tx, "%lld", x->ival); emit_fbits(tx, sz == 4); }
        else fprintf(out, "\t.%s %lld\n", sz == 1 ? "byte" : sz == 2 ? "short" : sz == 4 ? "long" : "quad", x->ival);
    }
    fprintf(out, "\t.section .rodata\n.Ltrue:\n\t.string \"true\"\n.Lfalse:\n\t.string \"false\"\n.Lempty:\n\t.string \"\"\n");
    for (int i = 0; i < nstr; i++) {
        fprintf(out, "\t.balign 8\n\t.quad -1, %d\n.LS%d:\n\t.byte ", strtab[i].n, i);
        for (int j = 0; j < strtab[i].n; j++) fprintf(out, "%d,", (unsigned char)strtab[i].d[j]);
        fprintf(out, "0\n");
    }
    for (int i = 0; i < nfc; i++) { fprintf(out, "\t.balign 8\n.LF%d:\n", i); emit_fbits(fconsts[i].text, fconsts[i].is32); }
    if (need_fneg) fprintf(out, "\t.balign 16\n.Lfneg64:\n\t.quad 0x8000000000000000, 0\n.Lfneg32:\n\t.long 0x80000000, 0, 0, 0\n");
    for (int i = 0; i < nvts; i++) {
        fprintf(out, ".Lvt%d:\n", i);
        for (int j = 0; j < vts[i].ifc->nm; j++) fprintf(out, "\t.quad ag_%s\n", mangle(find_method(vts[i].sd, strrchr(vts[i].ifc->m[j]->name, '.') + 1)->name));
    }
    fprintf(out, "\t.section .svalinn.caps,\"\",@progbits\n\t.asciz \"module=%s;allow=", mod_name);
    for (int i = 0; i < nallow; i++) fprintf(out, "%s%s", i ? "," : "", allow_caps[i]);
    fprintf(out, ";deny=");
    for (int i = 0; i < ndeny; i++) fprintf(out, "%s%s", i ? "," : "", deny_caps[i]);
    fprintf(out, ";protect=%s\"\n\t.section .note.GNU-stack,\"\",@progbits\n", protect_process ? "Process" : "");
}

/* ============================ AST dump (for checking the Svalinn front end) ============================
 * --dump-ast prints the parsed tree; --dump-sema prints it again after semantic analysis with types,
 * stack offsets and frame sizes attached (dump_typed = 1). */
static int dump_typed;
static const char *opname(int op) {
    static const char *n[] = { "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "==", "!=", "<", "<=", ">", ">=", "&&", "||", "neg", "!", "~", "deref", "addr" };
    return n[op];
}
static void d_ty(Expr *e) { if (dump_typed) printf(" :%s", e->ty ? tname(e->ty) : "?"); }
static void d_expr(Expr *e) {
    switch (e->k) {
    case E_INT:
        if (e->ty == &ty_char && !dump_typed) printf("(char %lld)", e->ival);
        else if (dump_typed && e->cls == 1) { printf("(char %lld", e->ival); d_ty(e); printf(")"); }
        else if (dump_typed && e->cls == 2) { printf("(null"); d_ty(e); printf(")"); }
        else if (e->cls == 2) printf("(null)");
        else if (e->cls == 1) printf("(char %lld)", e->ival);
        else { printf("(int %lld", e->ival); d_ty(e); printf(")"); }
        break;
    case E_BOOL: printf("(bool %lld", e->ival); d_ty(e); printf(")"); break;
    case E_FLOAT: printf("(float %s", e->sval); d_ty(e); printf(")"); break;
    case E_STR:
        printf("(str \"");
        for (int i = 0; i < e->slen; i++) { unsigned char c = (unsigned char)e->sval[i]; if (c >= 32 && c < 127 && c != '"' && c != '\\') printf("%c", c); else printf("\\x%02x", c); }
        printf("\""); d_ty(e); printf(")"); break;
    case E_VAR:
        printf("(var %s", e->name);
        if (dump_typed && e->lv) { if (e->lv->is_global) printf(" @g"); else printf(" @%c%d", e->lv->is_ref ? 'r' : 'l', e->lv->off); }
        if (dump_typed && e->move) printf(" mv");
        if (dump_typed && e->copy) printf(" cp");
        d_ty(e); printf(")"); break;
    case E_CALL:
        printf("(call %s", e->name);
        if (dump_typed) { if (e->builtin) printf(" b%d", e->builtin); if (e->tmp) printf(" t%d", e->tmp); if (e->dflag) printf(" f%d", e->dflag); }
        for (int i = 0; i < e->nargs; i++) { printf(" "); d_expr(e->args[i]); }
        d_ty(e); printf(")"); break;
    case E_BIN: printf("(%s ", opname(e->op)); d_expr(e->a); printf(" "); d_expr(e->b); d_ty(e); printf(")"); break;
    case E_UN: printf("(%s ", opname(e->op)); d_expr(e->a); d_ty(e); printf(")"); break;
    case E_CAST: printf("(as "); d_expr(e->a); printf(" %s", tname(e->cast_ty)); d_ty(e); printf(")"); break;
    case E_INDEX: printf("(index "); d_expr(e->a); printf(" "); d_expr(e->b); d_ty(e); printf(")"); break;
    case E_FIELD: printf("(. "); d_expr(e->a); printf(" %s", e->name); if (dump_typed && e->fld) printf(" +%d", e->fld->off); d_ty(e); printf(")"); break;
    case E_REF: printf("(ref "); d_expr(e->a); d_ty(e); printf(")"); break;
    case E_MCALL: printf("(mcall "); d_expr(e->a); printf(" %s", e->name); for (int i = 0; i < e->nargs; i++) { printf(" "); d_expr(e->args[i]); } printf(")"); break;
    case E_OK: case E_ERR:
        if (e->ty && e->ty->opt) printf(e->k == E_OK ? "(some" : "(none"); else printf(e->k == E_OK ? "(ok" : "(err");
        if (e->a) { printf(" "); d_expr(e->a); }
        if (dump_typed && e->tmp) printf(" t%d", e->tmp);
        if (dump_typed && e->dflag) printf(" f%d", e->dflag);
        d_ty(e); printf(")"); break;
    case E_TRY: printf("(try "); d_expr(e->a); d_ty(e); printf(")"); break;
    case E_AWAIT: printf("(await "); d_expr(e->a); if (dump_typed && e->tmp) printf(" t%d", e->tmp); if (dump_typed && e->dflag) printf(" f%d", e->dflag); d_ty(e); printf(")"); break;
    case E_TOIFACE: printf("(toiface "); d_expr(e->a); printf(" vt%lld", e->ival); if (dump_typed && e->tmp) printf(" t%d", e->tmp); d_ty(e); printf(")"); break;
    case E_SIZEOF: printf("(sizeof %s)", tname(e->cast_ty)); break;
    case E_ENUMVAL: printf("(enumval %s.%s %lld", e->a->name, e->name, e->ival); d_ty(e); printf(")"); break;
    case E_VARIANT:
        printf("(variant %s.%s %lld", e->ty->ed->name, e->ty->ed->vn[e->ival], e->ival);
        for (int i = 0; i < e->nargs; i++) { printf(" "); d_expr(e->args[i]); }
        if (dump_typed && e->tmp) printf(" t%d", e->tmp);
        if (dump_typed && e->dflag) printf(" f%d", e->dflag);
        d_ty(e); printf(")"); break;
    case E_SLIT:
        printf("(slit %s", e->name);
        for (int i = 0; i < e->nargs; i++) { printf(" (init %s ", e->args[i]->name); d_expr(e->args[i]->a); printf(")"); }
        if (dump_typed && e->tmp) printf(" t%d", e->tmp);
        if (dump_typed && e->dflag) printf(" f%d", e->dflag);
        d_ty(e); printf(")"); break;
    default: printf("(?)"); break;
    }
}
static void d_expr_opt(Expr *e) { if (e) d_expr(e); else printf("_"); }
static void d_stmt(Stmt *s) {
    switch (s->k) {
    case S_LET:
        printf("(let %s %s ", s->name, s->is_mut ? "var" : "let"); if (s->dty) printf("%s", tname(s->dty)); else printf("_"); printf(" "); d_expr_opt(s->e);
        if (dump_typed && s->lv) printf(" @%d", s->lv->off);
        if (dump_typed && s->lv && s->lv->dflag) printf(" d%d", s->lv->dflag);
        printf(")"); break;
    case S_ASSIGN: printf("(assign "); d_expr(s->e); printf(" "); d_expr(s->e2); printf(")"); break;
    case S_EXPR: printf("(expr "); d_expr(s->e); printf(")"); break;
    case S_IF: printf("(if "); d_expr(s->e); printf(" "); d_stmt(s->body); printf(" "); if (s->els) d_stmt(s->els); else printf("_"); printf(")"); break;
    case S_WHILE: printf("(while "); d_expr(s->e); printf(" "); d_stmt(s->body); printf(")"); break;
    case S_FOR:
        printf("(for %s ", s->name); d_expr(s->e); printf(" "); if (s->is_range) d_expr(s->e2); else printf("_"); printf(" "); d_stmt(s->body);
        if (dump_typed && s->lv) printf(" @%d @%d", s->lv->off, s->slot1);
        printf(")"); break;
    case S_RET: printf("(return "); d_expr_opt(s->e); printf(")"); break;
    case S_BRK: printf("(break)"); break;
    case S_CONT: printf("(continue)"); break;
    case S_BLOCK: printf(s->unsafe_blk ? "(unsafe" : "(block"); for (int i = 0; i < s->n; i++) { printf(" "); d_stmt(s->list[i]); } printf(")"); break;
    case S_MATCH:
        printf("(match "); d_expr(s->e);
        if (dump_typed) printf(" @%d", s->slot1);
        for (int i = 0; i < s->n; i++) {
            printf(" (arm "); d_expr_opt(s->pats[i]);
            if (dump_typed && s->alv && s->alv[i]) printf(" @%d", s->alv[i]->off);
            if (dump_typed && s->vlv && s->vlv[i]) for (int j = 0; j < MAXVF; j++) if (s->vlv[i][j]) printf(" @%d", s->vlv[i][j]->off);
            printf(" "); d_stmt(s->list[i]); printf(")");
        }
        printf(")"); break;
    }
}
static void dump_ast(void) {
    printf("(module %s)\n", mod_name);
    for (int i = 0; i < nallow; i++) printf("(allow %s)\n", allow_caps[i]);
    for (int i = 0; i < ndeny; i++) printf("(deny %s)\n", deny_caps[i]);
    if (protect_process) printf("(protect)\n");
    for (int i = 0; i < nstructs; i++) {
        StructDef *sd = structs[i]; printf(sd->is_class ? "(class %s" : "(struct %s", sd->name);
        if (sd->base) printf(" extends %s", sd->base->name);
        for (int j = 0; j < sd->nifs; j++) printf(" implements %s", sd->ifs[j]->name);
        for (int j = 0; j < sd->nf; j++) {
            Field *fl = &sd->f[j]; printf(" (field %s %s", fl->name, tname(fl->ty));
            if (sd->is_class) { printf(" %s %s", vis_name(fl->vis), fl->is_mut ? "var" : "let"); if (fl->init) { printf(" "); d_expr(fl->init); } }
            if (dump_typed) printf(" +%d", fl->off);
            printf(")");
        }
        if (dump_typed) printf(" size=%d align=%d", sd->ty->size, sd->ty->align);
        printf(")\n");
    }
    for (int i = 0; i < nifaces; i++) printf("(interface %s)\n", ifaces[i]->name);
    for (int i = 0; i < nenums; i++) { printf("(enum %s", enums[i]->name); for (int j = 0; j < enums[i]->nv; j++) printf(" %s", enums[i]->vn[j]); printf(")\n"); }
    for (int i = 0; i < nglobals; i++) {
        Local *g = globals[i]; printf("(global %s %s ", g->name, g->is_const ? "const" : g->is_mut ? "var" : "let");
        if (g->ty) printf("%s", tname(g->ty)); else printf("_");
        printf(" "); d_expr_opt(g->init); printf(")\n");
    }
    for (int i = 0; i < nfuncs; i++) {
        Func *f = funcs[i]; printf("(fn %s %s", f->name, f->is_extern ? "extern" : f->is_unsafe ? "unsafe" : "safe");
        if (f->is_async) printf(" async");
        if (f->is_test) printf(" test");
        if (f->is_deprecated) printf(" deprecated");
        if (f->ifc) printf(" abstract");
        if (f->cls) printf(" %s %s", f->is_ctor ? "ctor" : f->is_dtor ? "dtor" : f->is_static ? "static" : "method", vis_name(f->vis));
        printf(" (");
        for (int j = 0; j < f->nparams; j++) {
            printf(" (param %s %s %s", f->pname[j], f->pref[j] ? "ref" : "val", tname(f->pty[j]));
            if (dump_typed && f->plocal) { printf(" @%d", f->plocal[j]->off); if (f->plocal[j]->tmp) printf(" t%d", f->plocal[j]->tmp); }
            printf(")");
        }
        printf(") %s ", tname(f->ret));
        for (int j = 0; j < f->nreq; j++) printf("(requires %s) ", f->req[j]);
        if (f->body) d_stmt(f->body); else printf("_");
        if (dump_typed && !f->is_extern && !f->ifc) printf(" frame=%d retslot=%d", f->frame, f->retslot);
        printf(")\n");
    }
}

/* ============================ imports / driver ============================ */
static char *read_file(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "svalinnc: cannot open '%s'\n", p); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = xalloc(n + 1); if (fread(b, 1, n, f) != (size_t)n) exit(1); fclose(f); return b;
}
static char *dir_of(const char *path) {
    const char *sl = strrchr(path, '/'); if (!sl) return xstrn(".", 1);
    return xstrn(path, sl - path ? (size_t)(sl - path) : 1);
}
static char *src_dir, *exe_dir; static char **loaded; static int nloaded, cloaded;
static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) { fclose(f); return 1; } return 0; }
static int file_exists(const char *p);
static void load_prelude(void) {   /* lib/rt/prelude.svl is part of every program (appended after the main module) */
    char path[1200]; snprintf(path, sizeof path, "%s/lib/rt/prelude.svl", exe_dir);
    if (!file_exists(path)) snprintf(path, sizeof path, "%s/../lib/rt/prelude.svl", exe_dir);
    if (!file_exists(path)) { fprintf(stderr, "svalinnc: cannot find the runtime prelude (lib/rt/prelude.svl)\n"); exit(1); }
    VPUSH(loaded, nloaded, cloaded, xstrn(path, strlen(path)));
    char *fp = "lib/rt/prelude.svl"; VPUSH(files, nfiles, cfiles, fp); rt_fidx = nfiles - 1; file_trusted[rt_fidx] = 1;
    Token *st = toks; int sn = ntoks, sc = ctoks; toks = 0; ntoks = ctoks = 0;
    lex(read_file(path), nfiles - 1);
    Token *nt = toks; int nn = ntoks; toks = st; ntoks = sn; ctoks = sc;
    int newn = ntoks - 1 + nn; Token *arr = xalloc(sizeof(Token) * (size_t)(newn + 1));
    memcpy(arr, toks, sizeof(Token) * (size_t)(ntoks - 1)); memcpy(arr + ntoks - 1, nt, sizeof(Token) * (size_t)nn);
    toks = arr; ntoks = newn; ctoks = newn + 1;
}
/* fn main(args: Vec<String>): a generated entry point (lib/rt/args.svl, not a real file) collects the command line */
static const char *MAIN_ARGS_SRC =
    "module rt.args;\nimport std.vec;\n"
    "unsafe fn __svalinn_main(argc: i32, argv: **u8) -> i32 {\n"
    "    var args = Vec<String>();\n"
    "    var i = 0;\n"
    "    while i < argc { args.push(*(argv + i) as String); i += 1; }\n";
static int tok_is(int i, int k, const char *s) { return toks[i].k == k && !strcmp(toks[i].s, s); }
static void load_main_args(void) {
    int depth = 0, found = 0, isvoid = 1;
    for (int i = 0; toks[i].k != TK_EOF && POS_FILE(toks[i].line) == 0; i++) {
        if (tok_is(i, TK_OP, "{")) depth++;
        else if (tok_is(i, TK_OP, "}")) depth--;
        else if (depth == 0 && tok_is(i, TK_ID, "fn") && tok_is(i + 1, TK_ID, "main") && tok_is(i + 2, TK_OP, "(") && toks[i + 3].k == TK_ID && !(i > 0 && tok_is(i - 1, TK_ID, "unsafe"))) {
            int j = i + 3; while (toks[j].k != TK_EOF && !tok_is(j, TK_OP, ")")) j++;
            found = 1; isvoid = !(toks[j].k != TK_EOF && tok_is(j + 1, TK_OP, "->"));
        }
    }
    if (!found) return;
    char src[1024]; snprintf(src, sizeof src, "%s%s}\n", MAIN_ARGS_SRC, isvoid ? "    main(args);\n    return 0;\n" : "    return main(args);\n");
    char *fp = "lib/rt/args.svl"; VPUSH(files, nfiles, cfiles, fp); file_trusted[nfiles - 1] = 1;
    Token *st = toks; int sn = ntoks, sc = ctoks; toks = 0; ntoks = ctoks = 0;
    lex(xstrn(src, strlen(src)), nfiles - 1);
    Token *nt = toks; int nn = ntoks; toks = st; ntoks = sn; ctoks = sc;
    int newn = ntoks - 1 + nn; Token *arr = xalloc(sizeof(Token) * (size_t)(newn + 1));
    memcpy(arr, toks, sizeof(Token) * (size_t)(ntoks - 1)); memcpy(arr + ntoks - 1, nt, sizeof(Token) * (size_t)nn);
    toks = arr; ntoks = newn; ctoks = newn + 1;
}
/* svalinnc --test: a generated runner (lib/rt/tests.svl, not a real file) runs every @test function of the main file
 * in its own process, so a failing test (panic, runtime error) does not stop the others */
static char *tsrc; static int tlen, tcap;
static void tcat(const char *a) { int n = (int)strlen(a); while (tlen + n + 1 > tcap) { tcap = tcap ? tcap * 2 : 4096; tsrc = realloc(tsrc, (size_t)tcap); } memcpy(tsrc + tlen, a, (size_t)n + 1); tlen += n; }
static void load_test_runner(void) {
    char *names[512]; int n = 0, depth = 0; char buf[400];
    for (int i = 0; toks[i].k != TK_EOF && POS_FILE(toks[i].line) == 0; i++) {
        if (tok_is(i, TK_OP, "{")) depth++;
        else if (tok_is(i, TK_OP, "}")) depth--;
        else if (depth == 0 && tok_is(i, TK_OP, "@") && tok_is(i + 1, TK_ID, "test")) {
            int j = i + 2; while (tok_is(j, TK_OP, "@") && toks[j + 1].k == TK_ID) j += 2;
            if (tok_is(j, TK_ID, "fn") && toks[j + 1].k == TK_ID && n < 512) names[n++] = toks[j + 1].s;
        }
    }
    tcat("module rt.tests;\nextern fn fork() -> i32;\nextern fn waitpid(pid: i32, status: *i32, options: i32) -> i32;\n"
         "extern fn _exit(code: i32);\nextern fn fflush(f: *u8) -> i32;\nfn __svalinn_call_test(i: i32) {\n");
    for (int k = 0; k < n; k++) { snprintf(buf, sizeof buf, "    if i == %d { %s(); }\n", k, names[k]); tcat(buf); }
    tcat("}\nfn __svalinn_run_test(i: i32) -> bool {\n    var st: i32 = 0;\n    unsafe {\n        fflush(null);\n"
         "        let pid = fork();\n        if pid == 0 { __svalinn_call_test(i); fflush(null); _exit(0); }\n"
         "        waitpid(pid, &st, 0);\n    }\n    return st == 0;\n}\nfn __svalinn_tests() -> i32 {\n    var failed = 0;\n");
    snprintf(buf, sizeof buf, "    println(\"running %d test%s\");\n", n, n == 1 ? "" : "s"); tcat(buf);
    for (int k = 0; k < n; k++) {
        snprintf(buf, sizeof buf, "    if __svalinn_run_test(%d) { println(\"test %s ... ok\"); } else { println(\"test %s ... FAILED\"); failed += 1; }\n", k, names[k], names[k]);
        tcat(buf);
    }
    snprintf(buf, sizeof buf, "    if failed == 0 { println(\"test result: ok. %d passed\"); return 0; }\n"
             "    println(\"test result: FAILED. \" + (%d - failed) + \" passed, \" + failed + \" failed\");\n    return 1;\n}\n", n, n);
    tcat(buf);
    char *fp = "lib/rt/tests.svl"; VPUSH(files, nfiles, cfiles, fp); file_trusted[nfiles - 1] = 1;
    Token *st = toks; int sn = ntoks, sc = ctoks; toks = 0; ntoks = ctoks = 0;
    lex(tsrc, nfiles - 1);
    Token *nt = toks; int nn = ntoks; toks = st; ntoks = sn; ctoks = sc;
    int newn = ntoks - 1 + nn; Token *arr = xalloc(sizeof(Token) * (size_t)(newn + 1));
    memcpy(arr, toks, sizeof(Token) * (size_t)(ntoks - 1)); memcpy(arr + ntoks - 1, nt, sizeof(Token) * (size_t)nn);
    toks = arr; ntoks = newn; ctoks = newn + 1;
}
static void expand_imports(void) {
    for (int i = 0; toks[i].k != TK_EOF; i++) {
        if (!(toks[i].k == TK_ID && !strcmp(toks[i].s, "import") && toks[i + 1].k == TK_ID)) continue;
        int line = toks[i].line, j = i + 1; char name[256] = "";
        for (;;) {
            if (toks[j].k != TK_ID) die(line, "malformed import");
            strncat(name, toks[j].s, 100); j++;
            if (toks[j].k == TK_OP && !strcmp(toks[j].s, ".")) { strcat(name, "/"); j++; continue; }
            break;
        }
        if (!(toks[j].k == TK_OP && !strcmp(toks[j].s, ";"))) die(line, "expected ';' after import");
        j++;
        char path[1200]; snprintf(path, sizeof path, "%s/%s.svl", src_dir, name); int from_lib = 0;
        if (!file_exists(path)) { snprintf(path, sizeof path, "%s/lib/%s.svl", exe_dir, name); from_lib = 1; }
        if (!file_exists(path)) { snprintf(path, sizeof path, "%s/../lib/%s.svl", exe_dir, name); from_lib = 1; }
        if (!file_exists(path)) die(line, "cannot find module '%s'", name);
        int dup = 0; for (int k = 0; k < nloaded; k++) if (!strcmp(loaded[k], path)) dup = 1;
        Token *nt = 0; int nn = 0;
        if (!dup) {
            VPUSH(loaded, nloaded, cloaded, xstrn(path, strlen(path)));
            char *fp = xstrn(path, strlen(path)); VPUSH(files, nfiles, cfiles, fp);
            if (from_lib && nfiles - 1 < 32) file_trusted[nfiles - 1] = 1;
            Token *st = toks; int sn = ntoks, sc = ctoks; toks = 0; ntoks = ctoks = 0;
            lex(read_file(path), nfiles - 1);
            nt = toks; nn = ntoks - 1; toks = st; ntoks = sn; ctoks = sc;
        }
        int newn = ntoks - (j - i) + nn; Token *arr = xalloc(sizeof(Token) * (size_t)(newn + 1));
        memcpy(arr, toks, sizeof(Token) * (size_t)i);
        if (nn) memcpy(arr + i, nt, sizeof(Token) * (size_t)nn);
        memcpy(arr + i + nn, toks + j, sizeof(Token) * (size_t)(ntoks - j));
        toks = arr; ntoks = newn; ctoks = newn + 1; i--;
    }
}
static void dump_tokens(void) {
    for (int i = 0; i < ntoks; i++) {
        Token *t = &toks[i]; int ln = POS_LINE(t->line);
        switch (t->k) {
        case TK_ID: printf("%d ID %s\n", ln, t->s); break;
        case TK_OP: printf("%d OP %s\n", ln, t->s); break;
        case TK_INT: printf("%d INT %llu\n", ln, t->v); break;
        case TK_FLOAT: printf("%d FLOAT %s\n", ln, t->s); break;
        case TK_CHAR: printf("%d CHAR %llu\n", ln, t->v); break;
        case TK_STR: printf("%d STR %d\n", ln, t->len); break;
        case TK_EOF: printf("%d EOF\n", ln); break;
        }
    }
}
static const char *USAGE =
    "usage: svalinnc <file.svl> [-o output] [-O] [-S]\n"
    "  -o output   name of the executable (default a.out; with -S, of the assembly file)\n"
    "  -O          optimize\n"
    "  -S          write assembly instead of an executable\n"
    "  --test      build a test runner that runs every @test function instead of main\n"
    "  --version   print the compiler version\n"
    "  --dump-tokens, --dump-ast, --dump-sema   print the compiler's internal stages\n";
int main(int argc, char **argv) {
    const char *in = 0, *outp = "a.out"; int asm_only = 0, dump = 0, dump_a = 0, dump_s = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = argv[++i];
        else if (!strcmp(argv[i], "-S")) asm_only = 1;
        else if (!strcmp(argv[i], "--dump-tokens")) dump = 1;
        else if (!strcmp(argv[i], "--dump-ast")) dump_a = 1;
        else if (!strcmp(argv[i], "--dump-sema")) dump_s = 1;
        else if (!strcmp(argv[i], "--test")) test_mode = 1;
        else if (!strcmp(argv[i], "-O")) opt = 1;
        else if (!strcmp(argv[i], "--version")) { printf("svalinnc " SVALINN_VERSION "\n"); return 0; }
        else if (argv[i][0] == '-') { fprintf(stderr, "%s", USAGE); return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 1; }
        else in = argv[i];
    }
    if (!in) { fprintf(stderr, "%s", USAGE); return 1; }
    g_path = in; src_dir = dir_of(in); exe_dir = dir_of(argv[0]);
    VPUSH(files, nfiles, cfiles, (char *)in);
    lex(read_file(in), 0);
    if (dump) { dump_tokens(); return 0; }
    load_prelude(); load_main_args(); if (test_mode) load_test_runner(); expand_imports(); prescan(); parse_program();
    if (dump_a) { dump_ast(); return 0; }
    sema();
    if (dump_s) { dump_typed = 1; dump_ast(); return 0; }
    {
        char rp[1200]; snprintf(rp, sizeof rp, "%s/lib/rt/fault.s", exe_dir);
        if (!file_exists(rp)) snprintf(rp, sizeof rp, "%s/../lib/rt/fault.s", exe_dir);
        if (!file_exists(rp)) { fprintf(stderr, "svalinnc: cannot find the runtime (lib/rt/fault.s)\n"); return 1; }
        rt_fault_text = read_file(rp);
    }
    if (uses_async) {
        char rp[1200]; snprintf(rp, sizeof rp, "%s/lib/rt/async.s", exe_dir);
        if (!file_exists(rp)) snprintf(rp, sizeof rp, "%s/../lib/rt/async.s", exe_dir);
        if (!file_exists(rp)) { fprintf(stderr, "svalinnc: cannot find the async runtime (lib/rt/async.s)\n"); return 1; }
        rt_async_text = read_file(rp);
    }
    char asmpath[1024]; if (asm_only) snprintf(asmpath, sizeof asmpath, "%s", outp); else snprintf(asmpath, sizeof asmpath, "%s.s", outp);
    FILE *o = fopen(asmpath, "w"); if (!o) { fprintf(stderr, "svalinnc: cannot write '%s'\n", asmpath); return 1; }
    gen_program(o); fclose(o);
    if (!asm_only) {
        char cmd[2200]; snprintf(cmd, sizeof cmd, "gcc -no-pie -o '%s' '%s' -lm", outp, asmpath);
        if (system(cmd) != 0) { fprintf(stderr, "svalinnc: assemble/link failed\n"); return 1; }
        remove(asmpath);
    }
    return 0;
}
