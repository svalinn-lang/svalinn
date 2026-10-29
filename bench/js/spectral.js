function a(i, j) { return 1.0 / ((i + j) * (i + j + 1) / 2 + i + 1); }
function mulAv(x, y) { const n = x.length; for (let i = 0; i < n; i++) { let s = 0; for (let j = 0; j < n; j++) s += a(i, j) * x[j]; y[i] = s; } }
function mulAtv(x, y) { const n = x.length; for (let i = 0; i < n; i++) { let s = 0; for (let j = 0; j < n; j++) s += a(j, i) * x[j]; y[i] = s; } }
const n = 2000; const u = new Float64Array(n).fill(1), v = new Float64Array(n), t = new Float64Array(n);
for (let i = 0; i < 10; i++) { mulAv(u, t); mulAtv(t, v); mulAv(v, t); mulAtv(t, u); }
let vbv = 0, vv = 0; for (let i = 0; i < n; i++) { vbv += u[i] * v[i]; vv += v[i] * v[i]; }
console.log(Math.sqrt(vbv / vv).toFixed(9));
