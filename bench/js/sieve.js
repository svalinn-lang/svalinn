const n = 20000000; const flags = new Uint8Array(n + 1); let count = 0;
for (let rep = 0; rep < 5; rep++) {
  flags.fill(1); count = 0;
  for (let i = 2; i <= n; i++) if (flags[i]) { count++; for (let j = i * i; j <= n; j += i) flags[j] = 0; }
}
console.log(count);
