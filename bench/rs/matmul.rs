fn main() {
    let n = 500usize;
    let mut a = vec![0.0f64; n * n]; let mut b = vec![0.0f64; n * n]; let mut c = vec![0.0f64; n * n];
    for i in 0..n { for j in 0..n { a[i * n + j] = ((i * 7 + j * 3) % 13) as f64 / 13.0; b[i * n + j] = ((i * 5 + j * 11) % 17) as f64 / 17.0; } }
    for _ in 0..3 {
        for x in c.iter_mut() { *x = 0.0; }
        for i in 0..n { for k in 0..n { let aik = a[i * n + k]; for j in 0..n { c[i * n + j] += aik * b[k * n + j]; } } }
    }
    let sum: f64 = c.iter().sum();
    println!("{:.6}", sum);
}
