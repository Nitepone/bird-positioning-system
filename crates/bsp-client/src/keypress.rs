//! "Request positioning" button. For now this is the Enter key on the
//! client's terminal; embedded clients will use a dedicated button.

use crate::uplink::Session;
use std::io::BufRead;
use std::sync::Arc;
use tokio::sync::mpsc;

pub fn spawn(session: Arc<Session>) {
    let (tx, mut rx) = mpsc::unbounded_channel();
    std::thread::Builder::new()
        .name("keypress".into())
        .spawn(move || {
            for line in std::io::stdin().lock().lines() {
                if line.is_err() || tx.send(()).is_err() {
                    return;
                }
            }
        })
        .expect("spawn keypress thread");
    tokio::spawn(async move {
        while rx.recv().await.is_some() {
            match session.request_positioning().await {
                Ok(()) => {
                    println!(">> Positioning requested; set this client's position in the web UI.")
                }
                Err(e) => {
                    println!(">> Positioning request failed: {e}");
                    session.recover(e).await;
                }
            }
        }
    });
}
