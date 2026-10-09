//! Keeps the microcontroller client's C core (`firmware/lib/bps_core`) in step
//! with the Rust code it was ported from: the clock-measurement packets and
//! `Estimator` in `bsp-proto`, and the noise gate's band-pass in `bsp-core`.
//! Everything here is tests; the C code is compiled by `build.rs`.

#[cfg(test)]
mod tests {
    use bsp_core::audio::BandPass;
    use bsp_proto::timesync::{Estimator, Request, Response, Sample};
    use bsp_proto::{NANOS_PER_SEC, Uuid};

    unsafe extern "C" {
        fn chk_est_new(window_ns: i64, bins: u32) -> *mut core::ffi::c_void;
        fn chk_est_free(e: *mut core::ffi::c_void);
        fn chk_est_push(
            e: *mut core::ffi::c_void,
            at: i64,
            offset_ns: i64,
            rtt_ns: i64,
            fit_at: *mut i64,
            offset: *mut f64,
            rate: *mut f64,
            fit_rtt: *mut i64,
            error: *mut i64,
            points: *mut u32,
        ) -> i32;
        fn chk_request_encode(seq: u32, id: *const u8, t1: i64, out: *mut u8);
        fn chk_response_decode(b: *const u8, len: usize, seq: *mut u32, t: *mut i64) -> i32;
        fn chk_sample_of(t: *const i64, t4: i64, out: *mut i64) -> i32;
        fn chk_bandpass(sample_rate: u32, low_hz: f32, high_hz: f32, pcm: *mut f32, n: usize);
    }

    /// The C estimator, behind the same interface as the Rust one.
    struct CEstimator(*mut core::ffi::c_void);

    impl CEstimator {
        fn new(window_ns: i64, bins: u32) -> Self {
            let e = unsafe { chk_est_new(window_ns, bins) };
            assert!(!e.is_null());
            Self(e)
        }

        /// (at, offset, rate, rtt, error, points)
        fn push(&mut self, s: Sample) -> Option<(i64, f64, f64, i64, i64, u32)> {
            let mut f = (0, 0.0, 0.0, 0, 0, 0);
            let ok = unsafe {
                chk_est_push(
                    self.0,
                    s.at,
                    s.offset_ns,
                    s.rtt_ns,
                    &mut f.0,
                    &mut f.1,
                    &mut f.2,
                    &mut f.3,
                    &mut f.4,
                    &mut f.5,
                )
            };
            (ok != 0).then_some(f)
        }
    }

    impl Drop for CEstimator {
        fn drop(&mut self) {
            unsafe { chk_est_free(self.0) }
        }
    }

    struct Rng(u64);

    impl Rng {
        fn next(&mut self) -> u64 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 7;
            self.0 ^= self.0 << 17;
            self.0
        }
        fn below(&mut self, n: u64) -> i64 {
            (self.next() % n) as i64
        }
    }

    /// A rough Wi-Fi-like measurement stream: drift, uneven delays in both
    /// directions, outliers, a clock step and a clock that steps back.
    fn stream(seed: u64, n: i64, interval_ns: i64) -> Vec<Sample> {
        let mut rng = Rng(seed);
        let mut out = Vec::new();
        let mut at = 1_700_000_000 * NANOS_PER_SEC;
        for i in 0..n {
            at += interval_ns + rng.below(2_000_000);
            if i == n * 3 / 4 {
                at -= 3 * NANOS_PER_SEC; // the client clock steps back
            }
            let mut offset = 1_200_000 + (15e-6 * (i * interval_ns) as f64) as i64;
            if i >= n / 2 {
                offset += 4_000_000; // and later steps 4 ms
            }
            let there = 100_000 + rng.below(5_000_000);
            let back = 100_000 + rng.below(5_000_000);
            let mut measured = offset + (there - back) / 2;
            if rng.below(50) == 0 {
                measured += 30_000_000; // a lone outlier
            }
            out.push(Sample {
                at,
                offset_ns: measured,
                rtt_ns: there + back,
            });
        }
        out
    }

    fn compare(window_ns: i64, bins: u32, samples: &[Sample]) {
        let mut rust = Estimator::new(window_ns, bins as usize);
        let mut c = CEstimator::new(window_ns, bins);
        for (i, &s) in samples.iter().enumerate() {
            let r = rust.push(s);
            let k = c.push(s);
            let (Some(r), Some(k)) = (r, k) else {
                assert_eq!(r.is_some(), k.is_some(), "sample {i}: fit presence differs");
                continue;
            };
            let what = format!("sample {i}: rust {r:?}, c {k:?}");
            assert_eq!(r.at, k.0, "{what}");
            assert_eq!(r.offset_ns.to_bits(), k.1.to_bits(), "{what}");
            assert_eq!(r.rate.to_bits(), k.2.to_bits(), "{what}");
            assert_eq!(r.rtt_ns, k.3, "{what}");
            assert_eq!(r.error_ns, k.4, "{what}");
            assert_eq!(r.points, k.5 as usize, "{what}");
        }
    }

    #[test]
    fn estimator_matches_rust_at_the_firmware_rate() {
        compare(64 * NANOS_PER_SEC, 16, &stream(1, 2_000, NANOS_PER_SEC / 2));
    }

    #[test]
    fn estimator_matches_rust_with_other_settings() {
        compare(
            64 * NANOS_PER_SEC,
            16,
            &stream(2, 3_000, NANOS_PER_SEC / 10),
        );
        compare(10 * NANOS_PER_SEC, 7, &stream(3, 1_000, NANOS_PER_SEC / 4));
        compare(30 * NANOS_PER_SEC, 1, &stream(4, 500, NANOS_PER_SEC));
    }

    #[test]
    fn packets_match() {
        let id = Uuid::new_v4();
        let req = Request {
            seq: 0xdead_beef,
            client_id: id,
            t1: -1_234_567_890_123,
        };
        let mut c = [0u8; 36];
        unsafe { chk_request_encode(req.seq, id.as_bytes().as_ptr(), req.t1, c.as_mut_ptr()) };
        assert_eq!(req.encode(), c);

        let resp = Response {
            seq: 77,
            t1: 1_000,
            t2: 5_000_000_000_000_000_000,
            t3: 5_000_000_000_000_000_100,
        };
        let b = resp.encode();
        let (mut seq, mut t) = (0u32, [0i64; 3]);
        assert_eq!(
            unsafe { chk_response_decode(b.as_ptr(), b.len(), &mut seq, t.as_mut_ptr()) },
            1
        );
        assert_eq!((seq, t), (77, [resp.t1, resp.t2, resp.t3]));
        assert_eq!(
            unsafe { chk_response_decode(req.encode().as_ptr(), 36, &mut seq, t.as_mut_ptr()) },
            0
        );

        for t4 in [500, 2_000, 9_000_000_000] {
            let r = Response {
                seq: 0,
                t1: 1_000,
                t2: 5_000,
                t3: 5_100,
            };
            let mut out = [0i64; 3];
            let ok = unsafe { chk_sample_of([r.t1, r.t2, r.t3].as_ptr(), t4, out.as_mut_ptr()) };
            match r.sample(t4) {
                Some(s) => assert_eq!((ok, out), (1, [s.at, s.offset_ns, s.rtt_ns])),
                None => assert_eq!(ok, 0),
            }
        }
    }

    #[test]
    fn gate_band_pass_matches_bsp_core() {
        let mut rng = Rng(9);
        let pcm: Vec<f32> = (0..48_000)
            .map(|i| {
                let t = i as f32 / 48_000.0;
                0.3 * (2.0 * std::f32::consts::PI * 3_500.0 * t).sin()
                    + 0.3 * (2.0 * std::f32::consts::PI * 80.0 * t).sin()
                    + (rng.below(1_000) as f32 / 1_000.0 - 0.5) * 0.05
            })
            .collect();
        for (sr, lo, hi) in [(48_000, 1_000.0, 10_000.0), (44_100, 800.0, 30_000.0)] {
            let mut f = BandPass::new(sr, lo, hi);
            let rust: Vec<f32> = pcm.iter().map(|&x| f.process(x)).collect();
            let mut c = pcm.clone();
            unsafe { chk_bandpass(sr, lo, hi, c.as_mut_ptr(), c.len()) };
            for (i, (a, b)) in rust.iter().zip(&c).enumerate() {
                assert!(
                    (a - b).abs() <= 1e-6,
                    "sample {i} at {sr} Hz: rust {a}, c {b}"
                );
            }
        }
    }
}
