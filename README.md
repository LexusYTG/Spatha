<div align="center">

> *"Mali dux, Mali bonum, Mali vita est."*
> ***Ave, Mali! Ave, ARM!***
❤️‍🔥

</div>

# Spatha

**Real Vulkan graphics for Linux containers on Android. No root required.**

---

## What it is

Spatha lets a Linux container (such as Ubuntu inside proot-distro) use your phone's actual GPU through Vulkan. It's made of two small programs that talk through a local socket:

| Part | Lives in | Job |
|---|---|---|
| **`spathad`** | The Android host | Talks to the phone's real Vulkan driver and runs what it's asked. |
| **`libspatha-icd.so`** | The Linux container | Looks like a normal Vulkan driver to apps, and passes every call to `spathad`. |

To the app, the GPU looks native.

## Why it exists

Android doesn't expose the GPU to containers the way a normal Linux PC does: there's no `/dev/dri`, and the driver is built for Android's own C library. So containers usually fall back to slow software rendering. Spatha gets around this without root and without touching system files, by letting the Android side do the GPU work and the container side just ask for it.

## What you get

- **Vulkan 1.0, 1.1 and 1.2**: the full everyday API, including graphics and compute pipelines, timeline semaphores, and buffer device address.
- **Output to an X11 window** (the container draws the finished frames into the window).
- **Fast frame delivery** through shared memory when the display supports it, with an automatic fallback when it doesn't.
- **Verified working:** a 19-test regression suite passes 19/19, and `vkcube`, `vkcubepp` and `vkmark` run end-to-end on X11.

## Performance

Measured with `vkmark` on a **Mali-G52 MC2**, on an otherwise idle device.

| Window size | Score | Notes |
|---|---:|---|
| 800×600 | 90 | Peaks around 108 FPS |
| 1280×720 | 56 | Steady ~60 FPS |

> **About the 720p numbers:** they sit at exactly 60 FPS across many different scenes while 800×600 reaches 108 FPS. That points to a frame cap somewhere in the display chain, not a GPU limit. Read the 720p result as a *minimum*, not a maximum. The cause is still under investigation.
>
> **1600×720** currently crashes after a few seconds. Likely a size limit in the driver's copy path; not yet diagnosed.

### Shared memory vs. plain copy

Spatha picks the fastest way to hand frames to the display on its own. Shared memory is faster, and the gap widens with resolution:

| Resolution | With shared memory | Without | Gain |
|---|---:|---:|---:|
| 640×360 | 83 | 59 | +41% |
| 854×480 | 67 | 54 | +24% |
| 1280×720 | 48 | 35 | +37% |
| 1920×1080 | 27 | 16 | +69% |

*(vkmark scores, one short run per cell. Set `SPATHA_NOSHM=1` to force the slower path.)*

## What was tested

| | |
|---|---|
| GPU | Mali-G52 MC2 |
| Driver | ARM proprietary (reports Vulkan 1.3.278) |
| Host | Android, using the system Vulkan loader |
| Container | Ubuntu (glibc, aarch64) in proot-distro |
| Environment | Gladiator + Termux:X11 |

## What it doesn't do

- **Vulkan 1.3.** The driver supports it, but Spatha only exposes up to 1.2.
- **Sparse resources, YCbCr conversion, protected memory**, and a few rarer 1.2 features.
- **Native window presentation.** Output only works through X11. No Wayland, no direct display.
- **Mailbox present mode.** Only FIFO and Immediate are available.
- **Sharing memory with Android buffers** (external memory, semaphores and fences).

## Where it's not a good fit

- **Any GPU other than the Mali-G52 MC2.** Adreno, PowerVR, other Mali models and others are untested.
- **Heavy workloads**, such as huge texture uploads or very high-frequency compute. Every call crosses a socket, which has a measurable cost.
- **Apps that require Vulkan 1.3**, such as recent Zink, DXVK or VKD3D-Proton.
- **Setups without Termux or without a prepared Linux container.**

## Known limits

- **Zink doesn't work on Android.** That's Mesa needing `/dev/dri`, which Android doesn't provide for Mali. It isn't a Spatha problem.
- **4K frames are rejected.** A 4K frame (~31.6 MB) exceeds the 16 MB message cap. 1080p fits. The cap can be raised, at the cost of more memory.
- **Mapped memory is copied.** Changes sync to the host when you submit, flush or unmap.
- **Commands are streamed, not batched.**

## How it fits together

```
Your app (in the container)
   → libspatha-icd.so       Vulkan driver stand-in
   → Unix socket
   → spathad                Android-side daemon
   → Android's Vulkan loader
   → Mali driver
   → GPU
```

## Repository layout

```
glibc/    the container-side driver   (built with gcc)
bionic/   the Android-side daemon     (built with clang)
build/    compiled binaries
```

## Building

```sh
# Android host (Termux)
cd bionic && make

# Container
cd glibc && make
```

Outputs: `build/bionic/spathad` and `build/glibc/libspatha-icd.so`.

## Installing

From the v1.2 release:

| File | Goes to |
|---|---|
| `libspatha-icd.so` | `/usr/lib/aarch64-linux-gnu/` in the container |
| `spatha_icd.json` | `/usr/share/vulkan/icd.d/` in the container |
| `spathad` | `$PREFIX/bin/` on the host |

Start the daemon on the host:

```sh
SPATHA_SOCK=$PREFIX/tmp/spatha.sock spathad
```

Then, in the container:

```sh
export SPATHA_SOCK=/host-tmp/spatha.sock
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/spatha_icd.json
```

## Settings

| Variable | Meaning |
|---|---|
| `SPATHA_SOCK` | Socket path. Default: `/tmp/spatha.sock`. |
| `SPATHA_ICD_DAEMON` | `off`, `try` or `require`. Default: `require`. |
| `SPATHA_DEBUG=1` | Print debug output. |
| `SPATHA_NOSHM=1` | Disable shared-memory presenting. |
| `SPATHA_VK_LIB` | *(daemon only)* Android Vulkan loader. Default: `/system/lib64/libvulkan.so`. |

## License

**GPL-3.0**. See `LICENSE`.
