# Notices for code contained in the DNG SDK subset

The Adobe agreement is not the only applicable notice. Preserve this file, the
full licence texts and the original source headers when redistributing the subset.

| Files | Additional terms and required treatment |
|---|---|
| `source/dng_safe_arithmetic.h` | Copyright (C) 2015 The Android Open Source Project; Apache License 2.0. The original attribution/licence header is preserved, and the complete licence is in `licenses/Apache-2.0.txt`. No upstream NOTICE file for this individual SDK component was supplied. |
| `source/dng_fingerprint.cpp` and `.h` | Derived from the RSA Data Security, Inc. MD5 Message-Digest Algorithm. Retain the full RSA notice embedded in both files, including identification of derivative works, permission and disclaimer. |
| `source/dng_lossless_jpeg_shared.cpp` | Adapted from the Independent JPEG Group's software and work by Brian C. Smith, the Regents of the University of California, Kongji Huang and Cornell University. Retain the complete copyright, permission and Cornell warranty/liability paragraphs in that file. |

This software is based in part on the work of the Independent JPEG Group.
The complete, unmodified IJG README supplied with the Adobe package is included
as `licenses/IJG-README.txt`: its LEGAL ISSUES section requires inclusion of that
README for source redistribution, identifies derivative-change reporting duties,
requires binary documentation acknowledgement, and limits author-name publicity.
It is retained in full to satisfy that requirement. Its other descriptions refer
to the IJG distribution, not to extra files included in this repository. No
changes have been made to the IJG-derived upstream file in this subset.

The MD5 support here is **derived from the RSA Data Security, Inc. MD5
Message-Digest Algorithm**. This identification is included to satisfy the
embedded RSA attribution terms. See the unchanged source headers for the entire
notice; no RSA-derived code is represented as newly authored by RawNode.

The full Apache-2.0 text is copied verbatim from the official package's Highway
LICENSE file. Only that standard licence text is reused here; no Highway code is
vendored in this directory.

## Separately fetched build dependencies

No source tree for these libraries is checked into this research change:

- LibRaw 0.22.3: LGPL 2.1/CDDL dual-licence distribution; inspect the selected
  distribution's `LICENSE.*`, `COPYRIGHT` and per-file notices before packaging.
- libjxl 0.11.2: BSD 3-clause project licence plus file-specific notices; retained
  in the separately fetched upstream tree.
- Highway 1.2.0: upstream Apache-2.0/BSD-3-Clause licensing, with its per-file
  licence notices and both licence texts retained in the fetched tree.
- Brotli 1.0.9: MIT licence; retained in the fetched tree.
- skcms: BSD 3-clause notice; retained in the fetched tree.
- System JPEG and zlib development libraries: their installed distributions'
  notices apply. A libjpeg-turbo package can have additional BSD/zlib/IJG terms.

Exact Git revisions and the LibRaw archive checksum are documented in
`research/jxl-dng-poc/REVIEW.md` and its preparation scripts. Fetching these
dependencies preserves their upstream licence files; it is not a claim that the
Adobe licence covers them. No XMP SDK, sample-photo licence or unrelated ZIP
component is bundled or relied upon for permission to redistribute this subset.
