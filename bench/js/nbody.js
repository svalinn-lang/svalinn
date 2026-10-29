const SOLAR_MASS = 39.47841760435743, DAYS = 365.24;
const B = [];
function body(x, y, z, vx, vy, vz, m) { return { x, y, z, vx: vx * DAYS, vy: vy * DAYS, vz: vz * DAYS, mass: m * SOLAR_MASS }; }
B.push(body(0, 0, 0, 0, 0, 0, 1));
B.push(body(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01, 1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05, 9.54791938424326609e-04));
B.push(body(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01, -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05, 2.85885980666130812e-04));
B.push(body(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01, 2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05, 4.36624404335156298e-05));
B.push(body(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01, 2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05, 5.15138902046611451e-05));
let px = 0, py = 0, pz = 0;
for (const b of B) { px += b.vx * b.mass; py += b.vy * b.mass; pz += b.vz * b.mass; }
B[0].vx = -px / SOLAR_MASS; B[0].vy = -py / SOLAR_MASS; B[0].vz = -pz / SOLAR_MASS;
function energy() {
  let e = 0;
  for (let i = 0; i < 5; i++) {
    const bi = B[i]; e += 0.5 * bi.mass * (bi.vx * bi.vx + bi.vy * bi.vy + bi.vz * bi.vz);
    for (let j = i + 1; j < 5; j++) { const bj = B[j]; const dx = bi.x - bj.x, dy = bi.y - bj.y, dz = bi.z - bj.z; e -= bi.mass * bj.mass / Math.sqrt(dx * dx + dy * dy + dz * dz); }
  }
  return e;
}
function advance(dt) {
  for (let i = 0; i < 5; i++) {
    const bi = B[i];
    for (let j = i + 1; j < 5; j++) {
      const bj = B[j]; const dx = bi.x - bj.x, dy = bi.y - bj.y, dz = bi.z - bj.z;
      const d2 = dx * dx + dy * dy + dz * dz, mag = dt / (d2 * Math.sqrt(d2)), mj = bj.mass * mag, mi = bi.mass * mag;
      bi.vx -= dx * mj; bi.vy -= dy * mj; bi.vz -= dz * mj; bj.vx += dx * mi; bj.vy += dy * mi; bj.vz += dz * mi;
    }
  }
  for (const b of B) { b.x += dt * b.vx; b.y += dt * b.vy; b.z += dt * b.vz; }
}
console.log(energy().toFixed(9));
for (let s = 0; s < 5000000; s++) advance(0.01);
console.log(energy().toFixed(9));
