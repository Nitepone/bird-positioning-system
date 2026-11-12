//! Which API versions of clients the server accepts (see `bsp_proto::API_VERSION`).
//!
//! The same major version is accepted: a client with a newer minor version
//! than the server's is too, but flagged, because it may send what the server
//! ignores. A client that leaves its version out predates API versions and is
//! accepted as compatible, but flagged. Any other major version is refused.

use bsp_proto::API_VERSION;
use serde::Serialize;
use std::str::FromStr;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ApiVersion {
    pub major: u32,
    pub minor: u32,
    pub patch: u32,
}

impl FromStr for ApiVersion {
    type Err = String;

    /// `MAJOR.MINOR.PATCH`, each a number.
    fn from_str(s: &str) -> Result<Self, String> {
        let bad = || format!("\"{s}\" is not a version of the form MAJOR.MINOR.PATCH");
        let mut parts = s.split('.').map(|p| {
            let digits = !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit());
            digits
                .then(|| p.parse::<u32>().ok())
                .flatten()
                .ok_or_else(bad)
        });
        let mut next = || parts.next().unwrap_or_else(|| Err(bad()));
        let v = Self {
            major: next()?,
            minor: next()?,
            patch: next()?,
        };
        if parts.next().is_some() {
            return Err(bad());
        }
        Ok(v)
    }
}

/// How a client's API version compares with the server's.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ApiCompat {
    /// The same major version, and a minor version no newer than the server's.
    Compatible,
    /// The same major version, but a newer minor: accepted, and flagged.
    ClientNewer,
    /// The client gave no version: old firmware. Accepted, and flagged.
    Legacy,
    /// Another major version: refused.
    Incompatible,
}

/// How a client that reports `client` (None: no version) fits a server that
/// implements `server`. An error if `client` is not a version.
pub fn check_against(server: ApiVersion, client: Option<&str>) -> Result<ApiCompat, String> {
    let Some(client) = client else {
        return Ok(ApiCompat::Legacy);
    };
    let c: ApiVersion = client.parse()?;
    Ok(if c.major != server.major {
        ApiCompat::Incompatible
    } else if c.minor > server.minor {
        ApiCompat::ClientNewer
    } else {
        ApiCompat::Compatible
    })
}

/// [`check_against`] for this server's own version.
pub fn check(client: Option<&str>) -> Result<ApiCompat, String> {
    check_against(
        API_VERSION.parse().expect("API_VERSION is a version"),
        client,
    )
}

/// The first API version with maintenance commands (reboot, trace logging).
pub const COMMANDS_SINCE: ApiVersion = ApiVersion {
    major: 1,
    minor: 1,
    patch: 0,
};

/// Whether a client that reports `client` takes maintenance commands: one of
/// the server's major version from [`COMMANDS_SINCE`] on. Old firmware (no
/// version) does not, and is never sent any.
pub fn takes_commands(client: Option<&str>) -> bool {
    let server: ApiVersion = API_VERSION.parse().expect("API_VERSION is a version");
    client
        .and_then(|c| c.parse::<ApiVersion>().ok())
        .is_some_and(|c| {
            c.major == server.major
                && (c.major, c.minor, c.patch)
                    >= (
                        COMMANDS_SINCE.major,
                        COMMANDS_SINCE.minor,
                        COMMANDS_SINCE.patch,
                    )
        })
}

/// The error for a client refused as [`ApiCompat::Incompatible`], naming both versions.
pub fn refusal(client: &str) -> String {
    format!(
        "incompatible API version: the client speaks {client} but the server implements \
         {API_VERSION}; their major versions must be the same, so update the client or the server"
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn server(s: &str) -> ApiVersion {
        s.parse().unwrap()
    }

    #[test]
    fn parses_versions() {
        let v = server("1.20.3");
        assert_eq!((v.major, v.minor, v.patch), (1, 20, 3));
        for bad in [
            "",
            "1",
            "1.0",
            "1.0.0.0",
            "1.x.0",
            "v1.0.0",
            "1.-1.0",
            "1..0",
            "1.0.0-rc1",
            " 1.0.0",
        ] {
            assert!(bad.parse::<ApiVersion>().is_err(), "{bad:?}");
        }
    }

    #[test]
    fn accepts_the_same_major() {
        let s = server("1.2.0");
        assert_eq!(check_against(s, Some("1.2.0")), Ok(ApiCompat::Compatible));
        assert_eq!(check_against(s, Some("1.0.7")), Ok(ApiCompat::Compatible));
        assert_eq!(check_against(s, Some("1.1.0")), Ok(ApiCompat::Compatible));
    }

    #[test]
    fn accepts_a_newer_minor_but_flags_it() {
        assert_eq!(
            check_against(server("1.0.0"), Some("1.3.0")),
            Ok(ApiCompat::ClientNewer)
        );
        // A newer patch changes nothing on the wire.
        assert_eq!(
            check_against(server("1.0.0"), Some("1.0.9")),
            Ok(ApiCompat::Compatible)
        );
    }

    #[test]
    fn refuses_another_major() {
        assert_eq!(
            check_against(server("1.4.0"), Some("2.0.0")),
            Ok(ApiCompat::Incompatible)
        );
        assert_eq!(
            check_against(server("2.0.0"), Some("1.9.9")),
            Ok(ApiCompat::Incompatible)
        );
        assert_eq!(
            check_against(server("1.0.0"), Some("0.9.0")),
            Ok(ApiCompat::Incompatible)
        );
    }

    #[test]
    fn a_missing_version_is_legacy_and_a_malformed_one_an_error() {
        assert_eq!(check_against(server("1.0.0"), None), Ok(ApiCompat::Legacy));
        assert!(check_against(server("1.0.0"), Some("one")).is_err());
    }

    #[test]
    fn the_refusal_names_both_versions() {
        let msg = refusal("2.0.0");
        assert!(msg.contains("2.0.0") && msg.contains(API_VERSION), "{msg}");
    }

    #[test]
    fn only_clients_from_1_1_take_commands() {
        assert!(!takes_commands(None));
        assert!(!takes_commands(Some("1.0.0")));
        assert!(!takes_commands(Some("1.0.9")));
        assert!(!takes_commands(Some("junk")));
        assert!(!takes_commands(Some("2.1.0")));
        assert!(takes_commands(Some("1.1.0")));
        assert!(takes_commands(Some("1.4.2")));
    }

    #[test]
    fn serialises_as_snake_case() {
        let json = serde_json::to_string(&ApiCompat::ClientNewer).unwrap();
        assert_eq!(json, "\"client_newer\"");
    }
}
