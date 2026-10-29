fn a(i: usize, j: usize) -> f64 { 1.0 / (((i + j) * (i + j + 1) / 2 + i + 1) as f64) }
fn mul_av(x: &[f64], y: &mut [f64]) { let n = x.len(); for i in 0..n { let mut s = 0.0; for j in 0..n { s += a(i, j) * x[j]; } y[i] = s; } }
fn mul_atv(x: &[f64], y: &mut [f64]) { let n = x.len(); for i in 0..n { let mut s = 0.0; for j in 0..n { s += a(j, i) * x[j]; } y[i] = s; } }
fn mul_atav(x: &[f64], y: &mut [f64], t: &mut [f64]) { mul_av(x, t); mul_atv(t, y); }
fn main() {
    let n = 2000;
    let mut u = vec![1.0; n]; let mut v = vec![0.0; n]; let mut t = vec![0.0; n];
    for _ in 0..10 { mul_atav(&u, &mut v, &mut t); mul_atav(&v, &mut u, &mut t); }
    let (mut vbv, mut vv) = (0.0, 0.0);
    for i in 0..n { vbv += u[i] * v[i]; vv += v[i] * v[i]; }
    println!("{:.9}", (vbv / vv).sqrt());
}
