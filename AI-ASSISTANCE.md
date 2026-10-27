<!-- Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved. -->

# AI assistance disclosure

This disclosure describes the use of AI tools in developing the project. The
maintainer should review it for accuracy and keep it current before using it
for a distribution submission.

## Development process

The maintainer defines the product requirements, features, and design
decisions. AI tools have been used to help implement those decisions, including
generating portions of source code, tests, and documentation, and to help
investigate and revise implementation issues. AI tools do not determine the
product direction or decide which changes are accepted.

AI-generated material is limited to portions of the project; this is not a
claim that every file or line was AI-generated. Human requirements, direction,
review, editing, and testing do not make AI-generated portions wholly
human-authored.

The following is a qualitative estimate of the AI-assisted material. It
describes areas of contribution, not a claim that all content in those areas
was AI-generated:

| Area | AI-assisted material | Approximate extent |
| --- | --- | --- |
| GTK Bluetooth application | Frontend implementation and feature-specific revisions | Substantial, iterative assistance; the maintainer defined requirements and design, then reviewed and tested the work. |
| COSMIC Bluetooth application | C++ engine, C ABI, Rust libcosmic frontend, Cargo-driven build, Docker support, and specification | Substantial, iterative assistance; the maintainer defined requirements and design, then reviewed and tested the work. |
| GUI core and tests | Feature-specific GUI core implementation and tests | Partial assistance; not every file or line in these areas was AI-generated. |
| Bluetooth architecture documentation | Explanations and revisions in selected public headers | Targeted additions and revisions. |
| Application specifications and build/test integration | GTK application specifications, CMake install-component changes, and related CI/test configuration | Targeted assistance across selected files. |

The maintainer should verify that these examples and estimates accurately
describe the material included in any release and its packaging, and update
them as needed.

## Other generated media

To the maintainer's current knowledge, no AI-generated multimedia assets
(such as images, audio, or video) are included in the application. Verify
this statement against the shipped assets before distribution.

## Review and verification

The maintainer reviews AI-assisted changes, runs regression tests, and performs
acceptance testing against the intended requirements and behavior before
deciding whether to accept the work. AI assistance may help with test
development or troubleshooting, but it does not replace the maintainer's
verification or acceptance decisions. The maintainer remains responsible for
the accepted code and its ongoing maintenance. Testing reduces risk but does
not guarantee that every defect has been found.
