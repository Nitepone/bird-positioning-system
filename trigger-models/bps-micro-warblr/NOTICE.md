# bps-micro-warblr

Trained by bps (`tools/train_detector.py`), 2026-11-05.

- Architecture: DrongoNet (Muhammad Mun'im Ahmad Zabidi et al., 2026,
  <https://github.com/mun3im/drongonet>), CC BY 4.0, re-implemented from its
  MODEL_REFERENCE.md.
- Training data: warblrb10k (DCASE 2018 Bird Audio Detection; Warblr /
  D. Stowell et al.), CC BY 4.0,
  <https://dcase.community/challenge2018/task-bird-audio-detection>; 80% of its
  clips, split by file id (`train_detector.py split`).
- Licence of this model: CC BY 4.0 <https://creativecommons.org/licenses/by/4.0/>.

Changes: trained from scratch on the data above; int8 quantised.
