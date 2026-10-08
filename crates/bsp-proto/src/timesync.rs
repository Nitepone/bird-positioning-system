//! NTP-style UDP clock measurement.
//!
//! Request (client -> server, 36 bytes): magic, version, kind=1, seq, client id, t1.
//! Response (server -> client, 36 bytes): magic, version, kind=2, seq, t1 (echoed), t2, t3.
//! t1 = client send, t2 = server receive, t3 = server send, t4 = client receive.

use crate::{ClientId, Timestamp};
use uuid::Uuid;

const MAGIC: [u8; 3] = *b"BSP";
const VERSION: u8 = 1;
const KIND_REQUEST: u8 = 1;
const KIND_RESPONSE: u8 = 2;

pub const REQUEST_LEN: usize = 3 + 1 + 1 + 3 + 4 + 16 + 8;
pub const RESPONSE_LEN: usize = 3 + 1 + 1 + 3 + 4 + 8 * 3;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Request {
    pub seq: u32,
    pub client_id: ClientId,
    pub t1: Timestamp,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Response {
    pub seq: u32,
    pub t1: Timestamp,
    pub t2: Timestamp,
    pub t3: Timestamp,
}

fn header(kind: u8, seq: u32) -> Vec<u8> {
    let mut b = Vec::with_capacity(RESPONSE_LEN);
    b.extend_from_slice(&MAGIC);
    b.push(VERSION);
    b.push(kind);
    b.extend_from_slice(&[0; 3]);
    b.extend_from_slice(&seq.to_be_bytes());
    b
}

fn check_header(b: &[u8], kind: u8, len: usize) -> Option<u32> {
    if b.len() != len || b[0..3] != MAGIC || b[3] != VERSION || b[4] != kind {
        return None;
    }
    Some(u32::from_be_bytes(b[8..12].try_into().ok()?))
}

fn i64_at(b: &[u8], at: usize) -> i64 {
    i64::from_be_bytes(b[at..at + 8].try_into().unwrap())
}

impl Request {
    pub fn encode(&self) -> Vec<u8> {
        let mut b = header(KIND_REQUEST, self.seq);
        b.extend_from_slice(self.client_id.as_bytes());
        b.extend_from_slice(&self.t1.to_be_bytes());
        b
    }

    pub fn decode(b: &[u8]) -> Option<Self> {
        let seq = check_header(b, KIND_REQUEST, REQUEST_LEN)?;
        let client_id = Uuid::from_slice(&b[12..28]).ok()?;
        Some(Self {
            seq,
            client_id,
            t1: i64_at(b, 28),
        })
    }
}

impl Response {
    pub fn encode(&self) -> Vec<u8> {
        let mut b = header(KIND_RESPONSE, self.seq);
        for t in [self.t1, self.t2, self.t3] {
            b.extend_from_slice(&t.to_be_bytes());
        }
        b
    }

    pub fn decode(b: &[u8]) -> Option<Self> {
        let seq = check_header(b, KIND_RESPONSE, RESPONSE_LEN)?;
        Some(Self {
            seq,
            t1: i64_at(b, 12),
            t2: i64_at(b, 20),
            t3: i64_at(b, 28),
        })
    }

    /// Returns `(offset_ns, rtt_ns)` given the client receive time `t4`.
    /// `offset` is server clock minus client clock.
    pub fn offset_rtt(&self, t4: Timestamp) -> (i64, i64) {
        let offset = ((self.t2 - self.t1) + (self.t3 - t4)) / 2;
        let rtt = (t4 - self.t1) - (self.t3 - self.t2);
        (offset, rtt)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let req = Request {
            seq: 7,
            client_id: Uuid::new_v4(),
            t1: 123_456_789,
        };
        assert_eq!(Request::decode(&req.encode()), Some(req));
        let resp = Response {
            seq: 7,
            t1: 1,
            t2: 2,
            t3: 3,
        };
        assert_eq!(Response::decode(&resp.encode()), Some(resp));
        assert_eq!(Response::decode(&req.encode()), None);
    }

    #[test]
    fn offset_math() {
        // Server is 5 ms ahead of client; one-way delay 1 ms each way; 0.2 ms processing.
        let off = 5_000_000;
        let t1 = 1_000_000_000;
        let t2 = t1 + 1_000_000 + off;
        let t3 = t2 + 200_000;
        let t4 = t3 - off + 1_000_000;
        let (o, rtt) = Response { seq: 0, t1, t2, t3 }.offset_rtt(t4);
        assert_eq!(o, off);
        assert_eq!(rtt, 2_000_000);
    }
}
