# RTX 3090 Linux DLSS / NGX proof of concept

This feature branch wires the current Pokemon Colosseum recomp release stack to
an experimental NVIDIA NGX Vulkan bootstrap. It is intentionally isolated from
`main`.

## Scope of this checkpoint

The first checkpoint proves that the release-pinned Colosseum host can:

1. keep the normal Vulkan path unchanged when NGX is not requested;
2. build against NVIDIA's public Linux DLSS/NGX SDK;
3. initialize NGX on the same Vulkan instance, physical device, and device that
   ModernGekko uses; and
4. cleanly fall back to the ordinary Vulkan renderer if NGX initialization
   fails.

This checkpoint does **not** evaluate DLSS Super Resolution yet, so no visual
difference is expected. An RTX 3090 is suitable for DLSS Super
Resolution/DLAA work, but not DLSS 5 3D-Guided Neural Rendering.

## 1. Verify the host

```sh
nvidia-smi
vulkaninfo --summary
```

The Vulkan device used for the test should be the RTX 3090.

## 2. Clone NVIDIA's public DLSS SDK

```sh
mkdir -p "$HOME/src"
git clone https://github.com/NVIDIA/DLSS.git "$HOME/src/NVIDIA-DLSS"
```

The checkout should contain:

```text
include/nvsdk_ngx_vk.h
lib/Linux_x86_64/libnvsdk_ngx.a
```

## 3. Clone the Colosseum feature branch

```sh
git clone --recurse-submodules \
  --branch feature/dlss-ngx-vulkan-poc \
  https://github.com/dougchansan/pkmn-colosseum-recomp.git

cd pkmn-colosseum-recomp
```

## 4. Build the real Colosseum package with NGX compiled in

Use the same legally obtained USA GC6E01 disc image used for the normal recomp.

```sh
export MODERNGEKKO_NGX_SDK_ROOT="$HOME/src/NVIDIA-DLSS"
sh ./setup.sh "/path/to/Pokemon-Colosseum-USA.iso" "$(nproc)"
```

The feature branch first applies the normal release host patches, then overlays
only the pinned Vulkan files from the reviewed NGX proof-of-concept commit.
The Colosseum CPU/runtime patches remain unchanged.

## 5. Baseline run

Do not request NGX yet:

```sh
cd build/release/PokemonColosseumRecomp
./PokemonColosseumRecomp
```

Select Vulkan and confirm the same scene/save used for later comparison boots
normally.

## 6. NGX handshake run

Launch the same package with the runtime opt-in:

```sh
MODERNGEKKO_DLSS=1 ./PokemonColosseumRecomp
```

Expected successful renderer log text contains:

```text
ModernGekko NGX: Vulkan SDK initialized on NVIDIA GeForce RTX 3090.
Phase 1 only validates the SDK/driver handshake; DLSS Super Resolution
evaluation is not enabled yet.
```

If NGX initialization fails, the normal Vulkan renderer should still continue.
NGX diagnostics are written under:

```text
$XDG_CACHE_HOME/moderngekko/ngx
```

or, if `XDG_CACHE_HOME` is unset:

```text
$HOME/.cache/moderngekko/ngx
```

ModernGekko's normal log remains in the portable Colosseum user directory.

## Evidence to keep

For the first host pass, capture:

- `nvidia-smi`;
- `vulkaninfo --summary`;
- the final setup/build result;
- a normal Vulkan boot without `MODERNGEKKO_DLSS`;
- the NGX-enabled boot with `MODERNGEKKO_DLSS=1`;
- the NGX initialization line or returned NGX error; and
- the NGX log directory if one is created.

No game files, generated recomp modules, saves, keys, or disc contents should
be committed or uploaded.

## Next implementation checkpoint

After the RTX 3090 handshake is confirmed, the next branch revision will:

1. negotiate NGX/DLSS Vulkan instance and device extensions before device
   creation;
2. query DLSS Super Resolution capability on the selected GPU;
3. expose the pre-UI XFB color image and depth information;
4. allocate an output-resolution Vulkan image;
5. add temporal jitter and a first camera-motion-vector buffer;
6. create/evaluate DLSS Super Resolution in Quality mode; and
7. compare output and GPU/frame timing against the native Vulkan baseline.

Per-object GX motion vectors and DLAA follow after that first visual DLSS pass.
DLSS 5 Neural Rendering stays a separate later provider/path because current
RTX 30-series hardware does not expose that feature.
