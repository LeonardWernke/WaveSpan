# Security policy

## Supported versions

WaveSpan is experimental. Security fixes target the `main` branch. If you found an issue on an older commit,
include that commit in your report.

## Report a vulnerability

Use [GitHub's private vulnerability form][report] to report a suspected vulnerability in WaveSpan.
If the form is unavailable, [open a public issue][issues] titled “Private security contact requested” and
include only a request for a private channel. Keep the vulnerability details private while the maintainers
investigate.

Reports can cover the launcher, build and extraction tools, shader translator, native runtime, local viewer,
or repository workflows. Examples include unintended code execution, unauthorized file access, exposure of
local data through the viewer, and tampering with build outputs.

Include what happened, the affected component and commit, your operating system, and any relevant tool or
driver versions. Describe the conditions needed to trigger the issue and what it could expose or change.
If possible, include a small hand-written input or a short, redacted log. A report is welcome even if you
cannot reproduce the issue on the latest commit.

Keep all game data on your PC, including executables, archives, saves, extracted assets, and shaders in any
form. Leave credentials, keys, personal data, and logs or dumps containing any of that material out of reports,
including private ones. Limit testing to systems and copies you control or have permission to use. Xbox and
Rockstar services, package decryption, and protection workarounds are outside the scope of this policy.

## Coordinated disclosure

Maintainers review reports privately and may ask for more information. If a vulnerability is confirmed,
maintainers coordinate a fix or mitigation and the timing of a public advisory with the reporter. Please
coordinate before publishing technical details. Maintainers can credit you in the advisory with your permission.

[report]: https://github.com/LeonardWernke/WaveSpan/security/advisories/new
[issues]: https://github.com/LeonardWernke/WaveSpan/issues/new
