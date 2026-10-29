fn main() {
    let n = 10usize;
    let mut perm = [0i32; 16]; let mut perm1 = [0i32; 16]; let mut count = [0i32; 16];
    let (mut max_flips, mut checksum, mut perm_count) = (0, 0, 0);
    for i in 0..n { perm1[i] = i as i32; }
    let mut r = n;
    loop {
        while r != 1 { count[r - 1] = r as i32; r -= 1; }
        perm[..n].copy_from_slice(&perm1[..n]);
        let mut flips = 0; let mut k = perm[0] as usize;
        while k != 0 {
            let (mut i, mut j) = (0, k);
            while i < j { perm.swap(i, j); i += 1; j -= 1; }
            flips += 1; k = perm[0] as usize;
        }
        if flips > max_flips { max_flips = flips; }
        if perm_count % 2 == 0 { checksum += flips; } else { checksum -= flips; }
        let mut done = true;
        while r != n {
            let p0 = perm1[0];
            for i in 0..r { perm1[i] = perm1[i + 1]; }
            perm1[r] = p0;
            count[r] -= 1;
            if count[r] > 0 { done = false; break; }
            r += 1;
        }
        if done { break; }
        perm_count += 1;
    }
    println!("{}\n{}", checksum, max_flips);
}
