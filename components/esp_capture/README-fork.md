# Local fork of espressif/esp_capture

Vendored (via `override_path` in `solutions/openai_demo/main/idf_component.yml`) for one reason:

**The AFE already computes a wake-word result, and the released component throws it away.**
`capture_audio_aec_src.c` fetches `afe_fetch_result_t` but reads only `data` and `ret_value`;
`wakeup_state` is never referenced anywhere in the component, so there is no public way to know
a wake word fired.

Changes, both additive and backward compatible:
- `esp_capture_audio_aec_src_cfg_t` gains `wake_cb` / `wake_ctx`.
- The fetch path forwards `res->wakeup_state == WAKENET_DETECTED` to that callback.

Nothing else is modified. `test_apps/` and `examples/` were dropped to keep the tree small.
To re-sync with a newer upstream release, re-copy the component and re-apply these two edits.
