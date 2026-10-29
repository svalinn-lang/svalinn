const n = 4000; let count = 0;
for (let y = 0; y < n; y++) {
  const ci = 2.0 * y / n - 1.0;
  for (let x = 0; x < n; x++) {
    const cr = 2.0 * x / n - 1.5; let zr = 0, zi = 0, inside = true;
    for (let i = 0; i < 50; i++) { const tr = zr * zr - zi * zi + cr, ti = 2.0 * zr * zi + ci; zr = tr; zi = ti; if (zr * zr + zi * zi > 4.0) { inside = false; break; } }
    if (inside) count++;
  }
}
console.log(count);
