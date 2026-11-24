The UE4SS build HSMMod carries: `UE4SS_v4.0.0.zip` from
https://github.com/UE4SS-RE/RE-UE4SS/releases/tag/v4.0.0, unchanged (MIT, its `ue4ss/LICENSE` is inside; sha256
`251ad0d1381a241750e036f5918cbf2b5d7ec58ebc608cb2dd09d459d0c81e45`, as the release lists it).
`src/ue4ss_bundle.cpp` embeds it. UE4SS's own `dwmapi.dll` in the zip is never unpacked.

To move to a newer build: replace the zip, change the file name in `src/ue4ss_bundle.cpp`, set `kBundledUe4ss` and
`kBundledUe4ssVersion` (the DLL's file version) in `shared/match/hsm_dll_rules.h` and add the hash to `kKnownUe4ss`.
Any other UE4SS.dll in `ue4ss/` is replaced by it.
`tests/test_ue4ss_bundle.py` checks the zip against the header. Keep the DLL under the updater's cap
(`kMaxUpdateDllBytes`): older clients refuse a bigger one.
