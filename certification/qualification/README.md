# Qualification and labels (SPEC §10)

`./ckt review` evaluates gates G1–G7 (`certification/qualification/gates.py`) against the **locked** `configs/gates.yaml` and assigns at most:

* **UNQUALIFIED**: any of G1, G2, G3, G6 or the synthetic part of G7 failed or was not evaluated.
* **SYNTHETICALLY_HARDENED**: G1, G2, G3, G5 and G6 pass, and the synthetic coverage plus holdout are complete.
* **DEVICE_VALIDATED**: additionally, the real-device corpus passes (`real_device_protocol.md`).
* **CERTIFICATION_CANDIDATE**: additionally, G4 passes on the named reference iOS device.

**CERTIFIED_TO_CKEY_PROFILE_<version>** is never produced by code. It requires:
1. All review notes in `agents/review_notes/`, with SIGN-OFF from R01–R11 and no open VETO.
2. A disposition document `certification/decisions/<date>-<version>.md` written by the independent certification reviewer (R12). It must state:
   * the scope (profiles, format versions, devices, OS versions);
   * the limits (minimum module size, contrast, lighting, motion);
   * the confidence intervals;
   * the exclusions;
   * the revocation rules.
3. That document signed with the reviewer's own key (`ssh-keygen -Y sign -n ckey-certification`), and the public key published with it.

Certification here means internal CKey conformance certification. It is not a third-party accreditation or regulatory certification.

**Revocation:** any later accepted wrong payload, or any regression found on a certified profile, revokes the label until the issue is fixed and the campaign replayed.
