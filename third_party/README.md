# Local SDK dependencies

Run `./scripts/fetch-sdks.ps1` from PowerShell to fetch and extract the exact
archives in `dependencies.lock.json`. Run it with `-VerifyOnly` to verify cached
archives and critical extracted files without network access or writes.

The script verifies SHA256 before extraction, retains existing SDK directories,
and refuses mismatched files. It runs no installers or SDK programs. Downloads
are cached in `.cache/`; the extracted SDKs and cache are not project source.

| Dependency | Pinned version | Relevant paths |
| --- | --- | --- |
| Intel XeSS SDK | 3.0.2, commit `8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0` | `xess/inc/xess_fg/`, `xess/bin/libxess_fg.dll`, `xess/bin/libxell.dll`, `xess/lib/libxess_fg.lib` |
| Khronos Vulkan-Headers | `vulkan-sdk-1.4.363.0`, commit `6802bb4733b63ed5efd3adb308a6c885ef180ea1` | `Vulkan-Headers/include/` |

XeSS's archive hash matches the SHA256 published for the official GitHub release
asset. Vulkan-Headers uses an official GitHub archive pinned to its full commit;
the lock records the SHA256 of the downloaded archive. No unsigned third-party
replacement DLL is included.

## License retention

Each SDK archive is extracted in full with its upstream notices. The Intel SDK
contains proprietary, redistributable binary components; this project does not
treat them as open-source code. Keep `xess/LICENSE.txt` with any redistribution
and preserve Intel's binaries without alteration. Use the documented public API.

Vulkan-Headers includes `LICENSE.md`, `LICENSES/Apache-2.0.txt`,
`LICENSES/MIT.txt`, and per-file notices; preserve them when redistributing.

Dependencies are downloaded for local development only. Downloading an SDK does
not establish game compatibility, produce an interpolated frame, or enable
XeSS-FG in Endfield.
