fn main() {
    let n: usize = 20_000_000;
    let mut flags = vec![false; n + 1];
    let mut count = 0u64;
    for _ in 0..5 {
        for f in flags.iter_mut() { *f = true; }
        count = 0;
        let mut i = 2;
        while i <= n {
            if flags[i] { count += 1; let mut j = i * i; while j <= n { flags[j] = false; j += i; } }
            i += 1;
        }
    }
    println!("{}", count);
}
