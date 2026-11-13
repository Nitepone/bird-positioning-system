//! Firmware builds for updating the microcontroller clients over the air:
//! reading the build information in an uploaded image, and choosing the
//! build each client is to run (API 1.2.0).

use crate::compat;
use crate::db::ClientRecord;
use bsp_proto::{
    BUILD_INFO_MARKER, BuildInfo, FirmwareOffer, MAX_BUILD_INFO_LEN, Timestamp, firmware_path,
};
use serde::{Deserialize, Serialize};

/// The first byte of an ESP32 app image.
const ESP_IMAGE_MAGIC: u8 = 0xE9;
/// Where an ESP32 app image has its `esp_app_desc_t` (just after the image
/// header and the first segment's header), which starts with this word.
const APP_DESC_OFFSET: usize = 32;
const APP_DESC_MAGIC: u32 = 0xABCD_5432;

/// The build information as embedded in an image (see
/// [`bsp_proto::BUILD_INFO_MARKER`]).
#[derive(Deserialize)]
struct EmbeddedBuild {
    hw: String,
    #[serde(default)]
    tag: Option<String>,
    commit: String,
    dirty: bool,
    describe: String,
    built: String,
}

/// Whether `s` is a UTC time to the second, `YYYY-MM-DDTHH:MM:SSZ` (so that
/// such times sort as text).
fn is_utc_second(s: &str) -> bool {
    const SHAPE: &[u8; 20] = b"0000-00-00T00:00:00Z";
    s.len() == SHAPE.len()
        && s.bytes().zip(SHAPE).all(|(c, &want)| match want {
            b'0' => c.is_ascii_digit(),
            _ => c == want,
        })
}

/// The build an ESP32 firmware image (`firmware.bin`) says it is, from its
/// one [`BUILD_INFO_MARKER`]. An error, saying what is wrong, for anything
/// else.
pub fn read_build_info(image: &[u8]) -> Result<BuildInfo, String> {
    let magic = image
        .get(APP_DESC_OFFSET..APP_DESC_OFFSET + 4)
        .map(|w| u32::from_le_bytes(w.try_into().unwrap()));
    if image.first() != Some(&ESP_IMAGE_MAGIC) || magic != Some(APP_DESC_MAGIC) {
        return Err(
            "not an ESP32 app image (expected firmware.bin from the build, not the merged \
                    flash image, an .elf or a Pico .uf2)"
                .into(),
        );
    }
    let mut found = image
        .windows(BUILD_INFO_MARKER.len())
        .enumerate()
        .filter(|(_, w)| *w == BUILD_INFO_MARKER)
        .map(|(i, _)| i);
    let Some(at) = found.next() else {
        return Err(
            "the image has no build information (BPS_BUILD_INFO): it was built without \
                    over-the-air update support"
                .into(),
        );
    };
    if found.next().is_some() {
        return Err("the image has more than one BPS_BUILD_INFO block".into());
    }
    let rest = &image[at + BUILD_INFO_MARKER.len()..];
    let text = rest[..rest.len().min(MAX_BUILD_INFO_LEN + 1)]
        .iter()
        .position(|&b| b == 0)
        .map(|end| &rest[..end])
        .ok_or_else(|| {
            format!("the build information is not NUL-terminated within {MAX_BUILD_INFO_LEN} bytes")
        })?;
    let b: EmbeddedBuild = serde_json::from_slice(text)
        .map_err(|e| format!("the build information is not valid JSON: {e}"))?;
    if b.hw.is_empty() || b.commit.is_empty() || b.describe.is_empty() {
        return Err("the build information leaves hw, commit or describe empty".into());
    }
    if !is_utc_second(&b.built) {
        return Err(format!(
            "the build time {:?} is not of the form YYYY-MM-DDTHH:MM:SSZ",
            b.built
        ));
    }
    Ok(BuildInfo {
        hardware_id: b.hw,
        tag: b.tag.filter(|t| !t.is_empty()),
        commit: b.commit,
        dirty: b.dirty,
        describe: b.describe,
        built_at: b.built,
    })
}

/// MD5 of `data` as 32 lowercase hex digits.
pub fn md5_hex(data: &[u8]) -> String {
    use md5::{Digest, Md5};
    Md5::digest(data)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// A firmware image the server keeps (in `firmware_dir`, as `<md5>.bin`).
#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct FirmwareBuild {
    /// Assigned by the server; the clients download it by this.
    pub id: i64,
    pub hardware_id: String,
    pub tag: Option<String>,
    pub commit: String,
    pub dirty: bool,
    pub describe: String,
    /// `YYYY-MM-DDTHH:MM:SSZ`.
    pub built_at: String,
    /// Bytes.
    pub size: u64,
    /// 32 lowercase hex digits.
    pub md5: String,
    pub uploaded_at: Timestamp,
}

impl FirmwareBuild {
    pub fn info(&self) -> BuildInfo {
        BuildInfo {
            hardware_id: self.hardware_id.clone(),
            tag: self.tag.clone(),
            commit: self.commit.clone(),
            dirty: self.dirty,
            describe: self.describe.clone(),
            built_at: self.built_at.clone(),
        }
    }

    pub fn is_stable(&self) -> bool {
        self.tag.is_some() && !self.dirty
    }

    /// Whether this is the build `b` (see [`BuildInfo::same_build`]).
    pub fn is(&self, b: &BuildInfo) -> bool {
        self.info().same_build(b)
    }

    pub fn file_name(&self) -> String {
        format!("{}.bin", self.md5)
    }

    pub fn offer(&self) -> FirmwareOffer {
        FirmwareOffer {
            firmware_build_id: self.id,
            firmware_url: firmware_path(self.id),
            firmware_size: self.size,
            firmware_md5: self.md5.clone(),
            firmware_describe: self.describe.clone(),
        }
    }
}

/// Which build the operator wants a client to run. A client none was chosen
/// for stays on the build it runs.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "policy", rename_all = "snake_case")]
pub enum FirmwarePolicy {
    /// The newest build (by build time) for the client's hardware.
    LatestDev,
    /// The newest stable build (tagged, clean tree) for its hardware.
    LatestStable,
    /// This build.
    Manual { build_id: i64 },
}

/// What became of the latest update of a client, as shown on its page.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum UpdateResult {
    /// It registered with the build it was to update to.
    Updated,
    Deferred,
    Failed,
    RolledBack,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct LastUpdate {
    /// When the server heard of it.
    pub at: Timestamp,
    pub result: UpdateResult,
    pub build_id: i64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub detail: Option<String>,
}

/// A client's update state, kept with it.
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct FirmwareState {
    /// Builds that failed or rolled back on the client: not offered to it
    /// again until its selection is set again.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub blocked: Vec<i64>,
    /// The build last offered, so that each new target is logged once.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub offered: Option<i64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last: Option<LastUpdate>,
}

/// The build a client running `running` is to run under `policy` (None: none
/// chosen, so the build it runs), among `builds`.
pub fn target<'a>(
    policy: Option<FirmwarePolicy>,
    running: Option<&BuildInfo>,
    builds: &'a [FirmwareBuild],
) -> Option<&'a FirmwareBuild> {
    let newest = |stable_only: bool| {
        let hw = &running?.hardware_id;
        builds
            .iter()
            .filter(|b| &b.hardware_id == hw && (!stable_only || b.is_stable()))
            .max_by(|a, b| (&a.built_at, a.id).cmp(&(&b.built_at, b.id)))
    };
    match policy {
        None => builds.iter().find(|b| running.is_some_and(|r| b.is(r))),
        Some(FirmwarePolicy::LatestDev) => newest(false),
        Some(FirmwarePolicy::LatestStable) => newest(true),
        Some(FirmwarePolicy::Manual { build_id }) => builds.iter().find(|b| b.id == build_id),
    }
}

/// Why the client cannot be updated over the air, or None if it can.
pub fn unsupported_reason(c: &ClientRecord) -> Option<String> {
    if !compat::speaks_firmware_updates(c.api_version.as_deref()) {
        return Some(format!(
            "its firmware speaks API {}; updates over the air need {}.{}.{} or later",
            c.api_version.as_deref().unwrap_or("unknown"),
            compat::FIRMWARE_SINCE.major,
            compat::FIRMWARE_SINCE.minor,
            compat::FIRMWARE_SINCE.patch
        ));
    }
    if !c.ota {
        return Some(
            "its firmware does not take updates over the air (only ESP32 boards do)".into(),
        );
    }
    if c.build.is_none() {
        return Some("it did not report which build it runs".into());
    }
    None
}

/// The build to offer the client in a heartbeat reply, if any: its target,
/// while that differs from what it runs, is for its hardware and has not
/// failed on it.
pub fn offer<'a>(c: &ClientRecord, builds: &'a [FirmwareBuild]) -> Option<&'a FirmwareBuild> {
    if unsupported_reason(c).is_some() {
        return None;
    }
    let running = c.build.as_ref()?;
    let t = target(c.firmware_policy, Some(running), builds)?;
    (t.hardware_id == running.hardware_id && !t.is(running) && !c.firmware.blocked.contains(&t.id))
        .then_some(t)
}

#[cfg(test)]
pub mod tests {
    use super::*;

    /// A minimal ESP32 app image around `info` (the text after the marker,
    /// without its NUL).
    pub fn image(info: &str) -> Vec<u8> {
        let mut img = vec![0u8; 256];
        img[0] = ESP_IMAGE_MAGIC;
        img[1] = 4; // segments
        img[APP_DESC_OFFSET..APP_DESC_OFFSET + 4].copy_from_slice(&APP_DESC_MAGIC.to_le_bytes());
        img.extend_from_slice(b"some rodata\0");
        img.extend_from_slice(BUILD_INFO_MARKER);
        img.extend_from_slice(info.as_bytes());
        img.push(0);
        img.extend_from_slice(&[0xff; 300]);
        img
    }

    pub fn info_json(
        hw: &str,
        tag: Option<&str>,
        commit: &str,
        dirty: bool,
        built: &str,
    ) -> String {
        let tag = tag.map_or("null".to_string(), |t| format!("\"{t}\""));
        let describe = format!("v1.0.0-{commit}");
        format!(
            r#"{{"hw":"{hw}","tag":{tag},"commit":"{commit}","dirty":{dirty},"describe":"{describe}","built":"{built}"}}"#
        )
    }

    #[test]
    fn reads_the_build_from_an_image() {
        let img = image(
            r#"{"hw":"esp32-s3 on xiao-proto-rev0.1","tag":"v1.3.0","commit":"3c48fb8","dirty":false,"describe":"v1.3.0","built":"2026-11-12T23:10:00Z"}"#,
        );
        let b = read_build_info(&img).unwrap();
        assert_eq!(
            b,
            BuildInfo {
                hardware_id: "esp32-s3 on xiao-proto-rev0.1".into(),
                tag: Some("v1.3.0".into()),
                commit: "3c48fb8".into(),
                dirty: false,
                describe: "v1.3.0".into(),
                built_at: "2026-11-12T23:10:00Z".into(),
            }
        );
        assert!(b.is_stable());
        let dev = read_build_info(&image(&info_json(
            "hw",
            None,
            "3c48fb8",
            true,
            "2026-11-12T23:10:00Z",
        )))
        .unwrap();
        assert!(!dev.is_stable() && dev.tag.is_none());
        // Tagged but dirty is not stable either.
        let mut tagged_dirty = dev.clone();
        tagged_dirty.tag = Some("v1".into());
        assert!(!tagged_dirty.is_stable());
    }

    #[test]
    fn refuses_what_it_cannot_use() {
        let ok = info_json("hw", None, "abc", false, "2026-11-12T23:10:00Z");
        let err = |img: &[u8]| read_build_info(img).unwrap_err();
        // Not an ESP32 app image.
        let mut img = image(&ok);
        img[0] = 0x7f;
        assert!(err(&img).contains("not an ESP32"));
        let mut img = image(&ok);
        img[APP_DESC_OFFSET] = 0;
        assert!(err(&img).contains("not an ESP32"));
        assert!(err(b"").contains("not an ESP32"));
        assert!(err(&[0xE9; 10]).contains("not an ESP32"));
        // No marker.
        let mut img = image(&ok);
        let at = img
            .windows(BUILD_INFO_MARKER.len())
            .position(|w| w == BUILD_INFO_MARKER)
            .unwrap();
        img[at] = b'X';
        assert!(err(&img).contains("no build information"));
        // Two.
        let mut img = image(&ok);
        img.extend_from_slice(BUILD_INFO_MARKER);
        img.extend_from_slice(ok.as_bytes());
        img.push(0);
        assert!(err(&img).contains("more than one"));
        // Bad JSON, missing keys, no NUL, a bad time.
        assert!(err(&image("{\"hw\":")).contains("not valid JSON"));
        assert!(err(&image(r#"{"hw":"x"}"#)).contains("not valid JSON"));
        let mut img = image(&ok);
        img.truncate(img.len() - 301); // the NUL and what follows
        img.extend_from_slice(&[b' '; 600]);
        assert!(err(&img).contains("NUL"));
        assert!(
            err(&image(&info_json(
                "x",
                None,
                "a",
                false,
                "2026-11-12 23:10"
            )))
            .contains("build time")
        );
        assert!(
            err(&image(&info_json(
                "",
                None,
                "a",
                false,
                "2026-11-12T23:10:00Z"
            )))
            .contains("empty")
        );
    }

    #[test]
    fn md5_is_lowercase_hex() {
        assert_eq!(md5_hex(b""), "d41d8cd98f00b204e9800998ecf8427e");
        assert_eq!(md5_hex(b"abc"), "900150983cd24fb0d6963f7d28e17f72");
    }

    fn build(id: i64, hw: &str, tag: Option<&str>, built: &str) -> FirmwareBuild {
        FirmwareBuild {
            id,
            hardware_id: hw.into(),
            tag: tag.map(Into::into),
            commit: format!("c{id}"),
            dirty: false,
            describe: format!("d{id}"),
            built_at: built.into(),
            size: 100,
            md5: format!("{id:032x}"),
            uploaded_at: 0,
        }
    }

    #[test]
    fn resolves_targets() {
        let builds = [
            build(1, "a", Some("v1"), "2026-01-01T00:00:00Z"),
            build(2, "a", None, "2026-03-01T00:00:00Z"),
            build(3, "a", Some("v2"), "2026-02-01T00:00:00Z"),
            build(4, "b", Some("v9"), "2026-05-01T00:00:00Z"),
        ];
        let running = builds[0].info();
        let t = |p| target(p, Some(&running), &builds).map(|b| b.id);
        assert_eq!(t(None), Some(1));
        assert_eq!(t(Some(FirmwarePolicy::LatestDev)), Some(2));
        assert_eq!(t(Some(FirmwarePolicy::LatestStable)), Some(3));
        assert_eq!(t(Some(FirmwarePolicy::Manual { build_id: 4 })), Some(4));
        assert_eq!(t(Some(FirmwarePolicy::Manual { build_id: 9 })), None);
        // A running build the server does not have.
        let mut other = running.clone();
        other.built_at = "2025-01-01T00:00:00Z".into();
        assert_eq!(target(None, Some(&other), &builds), None);
        assert_eq!(target(Some(FirmwarePolicy::LatestDev), None, &builds), None);
    }

    #[test]
    fn policies_have_a_wire_format() {
        let p: FirmwarePolicy =
            serde_json::from_str(r#"{"policy":"manual","build_id":3}"#).unwrap();
        assert_eq!(p, FirmwarePolicy::Manual { build_id: 3 });
        assert_eq!(
            serde_json::to_value(FirmwarePolicy::LatestStable).unwrap(),
            serde_json::json!({ "policy": "latest_stable" })
        );
    }
}
