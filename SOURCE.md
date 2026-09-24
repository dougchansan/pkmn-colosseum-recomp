# Corresponding source

The launcher artifact contains no Pokémon Colosseum disc data or generated game
module. In a binary artifact, `SOURCE_REVISIONS.txt` records the matching
project commit and submodules. Clone that commit with its recursive submodules,
then follow `README.md` and `setup.sh` or `setup.ps1` using your own supported
USA disc dump.

The source tree contains the host patch bundle under `mods/host-patches`, exact
upstream submodule revisions, build scripts, and the GPL license and notices.
If the referenced source commit is not accessible, do not distribute the binary
artifact until its corresponding source is available.
