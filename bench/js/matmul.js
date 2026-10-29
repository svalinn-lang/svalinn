const n = 500; const a = new Float64Array(n * n), b = new Float64Array(n * n), c = new Float64Array(n * n);
for (let i = 0; i < n; i++) for (let j = 0; j < n; j++) { a[i * n + j] = ((i * 7 + j * 3) % 13) / 13.0; b[i * n + j] = ((i * 5 + j * 11) % 17) / 17.0; }
for (let rep = 0; rep < 3; rep++) {
  c.fill(0);
  for (let i = 0; i < n; i++) for (let k = 0; k < n; k++) { const aik = a[i * n + k]; for (let j = 0; j < n; j++) c[i * n + j] += aik * b[k * n + j]; }
}
let sum = 0; for (let i = 0; i < n * n; i++) sum += c[i];
console.log(sum.toFixed(6));
