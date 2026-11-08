# iOS runner (device-side; not executed by the server campaign)

The server campaign runs the production C++ core, which the iOS app also runs, on simulated 4K and 1080p luma frames. Two things need a physical device:

1. **G4 latency.** Run the same frames through the real `CKeyDecoder` on the reference iPhone. Plan:
   * Add an XCTest target to `clearkeysolutions/ckey` (`sdk/swift`) that loads PNG frames exported by `./ckt repro` (or a curated set from `campaign/images/`).
   * Call `CKeyDecoder.decode(cgImage:)` on each frame and record `os_signpost` intervals.
   * Write one JSON line per frame: case_id, decode_ms, payload hash.
   This belongs in the production repo as a PR, so it is described here rather than vendored.
2. **Real-camera corpus.** See `certification/qualification/real_device_protocol.md`.

Export frames for a device run (the frames land in `campaign/repro/<case_id>/frame0.png`):

```bash
./ckt repro structured-S03-P2-b1-swe-00123
```
