# slor-gfx900

llama.cpp on AMD Instinct MI25 cards (Vega 10, gfx900) through ROCm/HIP: patches with gfx900 kernels and a HIP
all-reduce for tensor split, a rebuild of Ubuntu's rocBLAS with gfx900 kernels, two small firmware-side kernel
modules for the MI25, and the boot setup we run.

The patches apply to [PrismML-Eng/llama.cpp](https://github.com/PrismML-Eng/llama.cpp) at tag
`prism-b10709-9a9394a`. The kernels target gfx900 in general (Vega 56/64, Vega Frontier Edition and Radeon Pro
WX 9100 use the same chip), but only the MI25 was tested. The firmware tools are MI25-only (PCI ID 0x6860).
A few shared pieces come from P100 work (credited below); no Pascal-only kernels are included.

## Tested hardware

- 2x MI25 16 GB, each capped at 200 W of its 300 W rating.
- One card in a PCIe 3.0 x16 slot; the other in a chipset PCIe 2.0 x4 slot without PCIe atomics. Tensor split
  works there too.
- Core i3-4170, 24 GB RAM, Ubuntu 26.04 (kernel 7.0), Ubuntu's ROCm 7.1 packages.

## Results

All at 262k context, both cards, `split-mode = tensor`. Read = prompt processing, write = generation.

| Model | Read (t/s) | Write (t/s) |
|---|---|---|
| Qwen3.8 27B Q6 | 274 on a 20k prompt (117 without the HIP all-reduce) | ~28 for code, ~18 for prose |
| Gemma 4 26B-A4B (QAT Q4_K_XL) | 723 on a 20k prompt | 46 |
| Qwen3.6 35B-A3B (Q4) | about 570 | about 50 |

Qwen3.8 27B writes with MTP drafts, which keep about 80% of their guesses for code and about 40% for prose.
One card, Gemma 4 12B: raising the cap from 110 W to 200 W took a 20k read from 261 to 411 t/s.

## Setup

1. Packages (Ubuntu 26.04):
   ```
   sudo apt install git cmake build-essential hipcc libamdhip64-dev libhipblas-dev librocblas-dev rocm-cmake rocminfo
   git clone --depth 1 https://github.com/superslor/slor-gfx900 && cd slor-gfx900
   ```
2. rocBLAS with gfx900 kernels. Ubuntu's `librocblas5` has none, but its source package still carries the
   vega10 kernel logic. The script downloads Ubuntu's rocblas 7.1.0-1ubuntu4 source from Launchpad (checksums
   checked), applies Ubuntu's own `debian/patches`, and builds it for gfx900 only with Ubuntu's flags:
   ```
   sudo apt install libamd-comgr-dev libblas-dev libdrm-dev libhsa-runtime-dev libmsgpack-cxx-dev librocm-smi-dev \
       libroctx-dev clang clang-tools llvm-21 gfortran pkg-rocm-tools python3-joblib python3-msgpack python3-venv python3-yaml
   gfx900/rocblas/build-rocblas.sh
   sudo cp -a gfx900/rocblas/rocblas-gfx900 /opt/
   ```
   llama.cpp finds it through two variables:
   `LD_LIBRARY_PATH=/opt/rocblas-gfx900/lib` and `ROCBLAS_TENSILE_LIBPATH=/opt/rocblas-gfx900/library`.
3. llama.cpp: `./build.sh` clones the pinned base (shallow), applies `patches/`, and builds `llama-server` and
   `test-backend-ops` into `llama.cpp/build-gfx900/bin`.
   ```
   ./build.sh
   sudo mkdir -p /opt/llama-gfx900 && sudo cp -a llama.cpp/build-gfx900/bin /opt/llama-gfx900/
   ```
   Your user needs the `render` and `video` groups to open the cards.
4. Firmware tools (MI25 only; skip if your cards already show 64 CUs and the power limit you want). Our cards
   booted with 56 of 64 CUs soft-locked and a 110 W limit (VBIOS 113-D0513500-S00). They may be ex-Google Stadia
   cards (Stadia's GPU had 56 CUs); other MI25s may not have this firmware.
   - `vega10-cu-unlock` clears the VBIOS's user CU mask just before amdgpu reads it, so all 64 CUs come up.
   - `vega10-avfs-keep` keeps AVFS (the chip's own voltages) on after a custom power table is loaded; amdgpu
     turns it off otherwise.
   ```
   sudo cp -r gfx900/firmware/vega10-cu-unlock /usr/src/vega10-cu-unlock-1.1
   sudo cp -r gfx900/firmware/vega10-avfs-keep /usr/src/vega10-avfs-keep-1.1
   sudo dkms install vega10-cu-unlock/1.1 && sudo dkms install vega10-avfs-keep/1.1
   sudo cp gfx900/firmware/vega10-cu-unlock.conf /etc/modprobe.d/
   ```
   If your initramfs loads amdgpu, rebuild it so it carries the modules too. After a reboot the kernel log shows
   `vega10_cu_unlock: ... user CU mask 0xc000 cleared` for each card.
5. Power table. Add `amdgpu.ppfeaturemask=0xfff7ffff` to `GRUB_CMDLINE_LINUX_DEFAULT`, run `sudo update-grub`,
   and reboot. Then make your own table from your card's: no VBIOS is flashed, and the table is loaded at every
   boot.
   ```
   sudo cat /sys/class/drm/card0/device/pp_table > card.pptable
   python3 gfx900/firmware/make-pptable.py card.pptable mi25-200w.pptable --watts 200 --soc-stock
   sudo install -D -m 644 -t /usr/local/lib/gpu-setup mi25-200w.pptable mi25-200w.orig.sha256
   ```
   The script sets the PowerTune socket, battery and small power limits (110 to 200 W on ours). With
   `--soc-stock` it also moves SoC clock levels 1-4 (720/847/960/1028 MHz) to the 950/1000/1050/1100 mV entries
   and memory clock levels 2-3 (800/945 MHz) to 1000/1050 mV: a stock MI25 VBIOS's values, 50 mV below ours. It
   refuses `--soc-stock` if your clock levels differ. `gpu-setup` loads a table only over the exact firmware
   table it was made from (the `.orig.sha256`).
6. Boot setup (`gfx900/setup/`): `gpu-setup` loads the table and caps every card at `CAP_W` (200 by default;
   set it in `/etc/default/gpu-setup`) before the router starts.
   ```
   sudo install -m 755 gfx900/setup/gpu-setup /usr/local/sbin/
   sudo cp gfx900/setup/gpu-setup.service gfx900/setup/llama-router.service /etc/systemd/system/
   sudo install -D -m 644 gfx900/setup/gemm-tune.conf /etc/systemd/system/llama-router.service.d/gemm-tune.conf
   sudo install -D -m 644 gfx900/setup/models.ini /etc/llama/models.ini   # set your model path
   sudo systemctl daemon-reload && sudo systemctl enable --now gpu-setup llama-router
   ```
   If the kernel log fills with `xgpu ai mailbox trans msg`, the 16 GB BAR was not assigned: add
   `pci=realloc=on pci=nocrs` to the kernel line.

Runtime environment (in the two unit files): `LD_LIBRARY_PATH`, `ROCBLAS_TENSILE_LIBPATH`,
`GGML_HIP_GEMM_TUNE_FILE` (where the rocBLAS autotune keeps its choices), `GGML_CUDA_FA_VEC_GQA=1`, and
`LLAMA_SPEC_SAMPLE_TEMP=1.0` with `LLAMA_SPEC_DRAFT_TOPK=20` (sampled MTP drafts).

**At your own risk.** The MI25 is passively cooled and needs strong airflow at 200 W. Watch `temp2_input`
(junction, critical at 105 °C) in the card's hwmon.

## Changes

Each patch's message carries its measurements. The variable after each change turns it off.

- `0001` Skip KV blocks that a unified KV cache fully masks (vec/tile kernels). No switch.
- `0002` Faster f32→f16 conversion and Q6_K/Q5_K→f16 dequantization for the prompt path: `GGML_CUDA_DEQ_FAST=0`.
- `0003` Vec attention: optional GQA head folding (from 16k keys), launched by its real KV step. Folding stays on unless `GGML_CUDA_FA_VEC_GQA=1`, which gfx900 wants.
- `0004` Tile attention reads q4_0/q8_0 K/V straight into shared memory, with no f16 copy of the cache: `GGML_CUDA_FA_TILE_Q4_0=0`.
- `0005` q6_K matvec for 1-3 columns: `GGML_CUDA_MMVQ_GCN=0` (row-pair kernels: `GGML_CUDA_MMVQ_GCN_PAIR=0`).
- `0006` Gated delta net (Qwen3.5/3.6/3.8 linear attention): `GGML_CUDA_GDN_GCN=0`.
- `0007` RMS norm at decode sizes: `GGML_CUDA_RMS_GCN=0`.
- `0008` get_rows and concat launch shapes: `GGML_CUDA_GET_ROWS_GCN64=0`, `GGML_CUDA_CONCAT_T_GCN=0`.
- `0009` Single-token f16 attention, D256 x 2-way GQA: `GGML_CUDA_FA_GCN_DECODE=0`.
- `0010` Skip masked blocks, with a leaner mask scan and combine: `GGML_CUDA_FA_GCN_SKIP=0`.
- `0011` Single-token attention, 16 heads on one KV head: `GGML_CUDA_FA_GCN_MQA=0`.
- `0012` rocBLAS solution autotune for prompt GEMMs, saved to `GGML_HIP_GEMM_TUNE_FILE`: `GGML_HIP_GEMM_TUNE=0`.
- `0013` GEMM attention through rocBLAS for head size >= 512, fp32, lazy rescale: `GGML_CUDA_FA_GEMM=0`.
- `0014` HIP all-reduce for tensor split: `GGML_CUDA_ALLREDUCE=none`.
- `0015` Sampled MTP drafts, speculative-sampling verify: off unless `LLAMA_SPEC_SAMPLE_TEMP` is set.
- `0016` MTP draft head over the first 81920 tokens: `LLAMA_MTP_DRAFT_VOCAB=0`. Catch-up keeps only the K/V: `LLAMA_MTP_CATCHUP_KV_ONLY=0`.
- `0017` A separate draft model gets its own micro-batch: `LLAMA_SPEC_DRAFT_UBATCH=0`.
- `0018` Op profiler: off unless `GGML_CUDA_OP_PROF=N`.
- `0019` test-backend-ops cases for the above.

## Credits

- [llama.cpp / ggml](https://github.com/ggml-org/llama.cpp) and the
  [PrismML fork](https://github.com/PrismML-Eng/llama.cpp) these patches build on.
- [Kmic-68/llama.cpp](https://github.com/Kmic-68/llama.cpp): the GEMM attention path, the tile kernel's q4_0
  loader, the vec kernel's GQA folding (all via qr0n's patch set below), sampled MTP drafts and the reduced draft
  head.
- [qr0n/p100-llama-cpp](https://github.com/qr0n/p100-llama-cpp): the separate H2D copy stream in the all-reduce,
  the vectorised conversion, and the packaging of Kmic-68's attention code.
- Ubuntu's ROCm packaging of rocBLAS (the source and patches the rocBLAS script builds), and AMD's rocBLAS and
  Tensile.

## License

Patches and scripts: MIT, like llama.cpp (see `LICENSE`). The kernel modules in `gfx900/firmware/` are
GPL-2.0, as declared in their sources.
