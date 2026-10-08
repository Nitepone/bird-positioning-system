//! Renders a small waveform image (SVG) for a detection clip.

use std::fmt::Write;

pub const WIDTH: usize = 300;
pub const HEIGHT: usize = 60;

/// Min/max envelope of `pcm` as an SVG, normalised to the clip's peak.
/// `highlight` is an optional `(start, end)` fraction of the clip (0..=1),
/// e.g. the identified call, drawn as a shaded band.
pub fn render_svg(pcm: &[f32], highlight: Option<(f64, f64)>) -> String {
    let peak = pcm.iter().fold(0.0f32, |m, x| m.max(x.abs())).max(1e-6);
    let mid = HEIGHT as f32 / 2.0;
    let mut svg = format!(
        r#"<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}">"#
    );
    svg.push_str(r##"<rect width="100%" height="100%" fill="#f4f4f4"/>"##);
    if let Some((a, b)) = highlight {
        let (a, b) = (a.clamp(0.0, 1.0), b.clamp(0.0, 1.0));
        let _ = write!(
            svg,
            r##"<rect x="{:.1}" y="0" width="{:.1}" height="{HEIGHT}" fill="#ffe9a8"/>"##,
            a * WIDTH as f64,
            ((b - a) * WIDTH as f64).max(1.0)
        );
    }
    let _ = write!(
        svg,
        r##"<line x1="0" y1="{mid}" x2="{WIDTH}" y2="{mid}" stroke="#ccc"/>"##
    );
    svg.push_str(r##"<path stroke="#246" stroke-width="1" d=""##);
    if !pcm.is_empty() {
        for x in 0..WIDTH {
            let (s, e) = (
                x * pcm.len() / WIDTH,
                ((x + 1) * pcm.len() / WIDTH).max(x * pcm.len() / WIDTH + 1),
            );
            let col = &pcm[s..e.min(pcm.len())];
            let (lo, hi) = col
                .iter()
                .fold((0.0f32, 0.0f32), |(lo, hi), &v| (lo.min(v), hi.max(v)));
            let y = |v: f32| mid - v / peak * (mid - 2.0);
            // At least one pixel tall so silence still shows as a line.
            let _ = write!(svg, "M{x}.5 {:.1}V{:.1}", y(hi), y(lo).max(y(hi) + 1.0));
        }
    }
    svg.push_str(r#""/></svg>"#);
    svg
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn renders_columns() {
        let pcm: Vec<f32> = (0..10_000).map(|i| (i as f32 / 20.0).sin()).collect();
        let svg = render_svg(&pcm, Some((0.25, 0.5)));
        assert!(svg.starts_with("<svg") && svg.ends_with("</svg>"));
        assert_eq!(svg.matches('M').count(), WIDTH);
        assert!(svg.contains(r#"x="75.0""#));
        assert!(render_svg(&[], None).ends_with("</svg>"));
    }
}
