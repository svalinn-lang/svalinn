const n = 10; const perm = new Int32Array(16), perm1 = new Int32Array(16), count = new Int32Array(16);
let maxFlips = 0, checksum = 0, permCount = 0;
for (let i = 0; i < n; i++) perm1[i] = i;
let r = n;
for (;;) {
  while (r !== 1) { count[r - 1] = r; r--; }
  for (let i = 0; i < n; i++) perm[i] = perm1[i];
  let flips = 0, k = perm[0];
  while (k !== 0) { for (let i = 0, j = k; i < j; i++, j--) { const t = perm[i]; perm[i] = perm[j]; perm[j] = t; } flips++; k = perm[0]; }
  if (flips > maxFlips) maxFlips = flips;
  checksum += permCount % 2 === 0 ? flips : -flips;
  let done = true;
  while (r !== n) {
    const p0 = perm1[0];
    for (let i = 0; i < r; i++) perm1[i] = perm1[i + 1];
    perm1[r] = p0;
    if (--count[r] > 0) { done = false; break; }
    r++;
  }
  if (done) break;
  permCount++;
}
console.log(checksum); console.log(maxFlips);
