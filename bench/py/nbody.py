import math
SOLAR_MASS = 39.47841760435743
DAYS = 365.24
def body(x, y, z, vx, vy, vz, m): return [x, y, z, vx * DAYS, vy * DAYS, vz * DAYS, m * SOLAR_MASS]
B = [body(0, 0, 0, 0, 0, 0, 1),
     body(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01, 1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05, 9.54791938424326609e-04),
     body(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01, -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05, 2.85885980666130812e-04),
     body(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01, 2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05, 4.36624404335156298e-05),
     body(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01, 2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05, 5.15138902046611451e-05)]
px = py = pz = 0.0
for b in B:
    px += b[3] * b[6]; py += b[4] * b[6]; pz += b[5] * b[6]
B[0][3] = -px / SOLAR_MASS; B[0][4] = -py / SOLAR_MASS; B[0][5] = -pz / SOLAR_MASS
def energy():
    e = 0.0
    for i in range(5):
        bi = B[i]
        e += 0.5 * bi[6] * (bi[3] * bi[3] + bi[4] * bi[4] + bi[5] * bi[5])
        for j in range(i + 1, 5):
            bj = B[j]
            dx = bi[0] - bj[0]; dy = bi[1] - bj[1]; dz = bi[2] - bj[2]
            e -= bi[6] * bj[6] / math.sqrt(dx * dx + dy * dy + dz * dz)
    return e
def advance(dt):
    for i in range(5):
        bi = B[i]
        for j in range(i + 1, 5):
            bj = B[j]
            dx = bi[0] - bj[0]; dy = bi[1] - bj[1]; dz = bi[2] - bj[2]
            d2 = dx * dx + dy * dy + dz * dz
            mag = dt / (d2 * math.sqrt(d2))
            mj = bj[6] * mag; mi = bi[6] * mag
            bi[3] -= dx * mj; bi[4] -= dy * mj; bi[5] -= dz * mj
            bj[3] += dx * mi; bj[4] += dy * mi; bj[5] += dz * mi
    for b in B:
        b[0] += dt * b[3]; b[1] += dt * b[4]; b[2] += dt * b[5]
print("%.9f" % energy())
for s in range(5000000): advance(0.01)
print("%.9f" % energy())
