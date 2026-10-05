# Dependencies and licenses

This source snapshot includes modified dependency files because the Kickle
frontend, controller defaults, display filters, achievements, and hook lifetime
fixes have not been upstreamed. They are ordinary directories, not submodules.

- `nesrecomp/`: mstan/nesrecomp, PolyForm Noncommercial 1.0.0.
  See `nesrecomp/LICENSE`. Its noncommercial conditions still apply.
- `ZeldaTemplate/recomp-ui/`: mstan/recomp-ui, MIT.
  See `ZeldaTemplate/recomp-ui/LICENSE` and embedded third-party notices.
- SDL2: bundled Windows development files, zlib license.
  See `nesrecomp/runner/external/SDL2/COPYING.txt`.
- Dear ImGui and other bundled libraries: preserve their individual licenses
  and attribution in the dependency directories.
- Fixed display filters: CPU adaptations of mstan/snesrecomp's CRT Soft,
  LCD Grid, Sharp, and Warm Composite. The original Mega Man X Recomp passes
  are public domain / CC0-1.0.

Kickle Cubicle artwork and game trademarks belong to their respective owners.
Menu artwork, icon, and sound were supplied for this project; they are not
relicensed under the dependency licenses. No ROM, ROM hack patch, generated
ROM byte table, or personal save data is included.

No additional license for the original Kickle-specific modifications is
assigned by this packaging step. The repository owner can choose those terms
before publication, while preserving the applicable upstream licenses.
