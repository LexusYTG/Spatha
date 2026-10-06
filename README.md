<div align="center">

> *"Mali dux, Mali bonum, Mali vita est."*
> ***Ave, Mali! Ave, ARM!***
❤️‍🔥

</div>

# Spatha

Puente Vulkan **sin root** entre Android bionic y containers glibc.

## Qué es

Dos binarios que hablan por un socket Unix:

- **spathad** (bionic, corre en el host Android) carga `/system/lib64/libvulkan.so` — el loader del sistema — que a su vez carga el driver de la GPU del dispositivo.
- **libspatha-icd.so** (glibc, corre en el container) se registra como ICD de Vulkan. Cada entry point que la app invoca se serializa al socket, el daemon lo ejecuta contra el driver real y devuelve el resultado.

El container ve la GPU nativa del dispositivo como si fuera suya, sin root, sin `/dev/dri`, sin tocar `/vendor`.

## Por qué existe

Dar acceso a aceleración Vulkan real a entornos glibc embebidos (proot-distro, Termux, chroot) que de otra forma quedan atados a llvmpipe o a emulación por software. Es el sustrato para correr apps gráficas de escritorio en Android sin depender de que Mesa traiga un backend DRM que Android no expone.

## Qué logra

- **Vulkan 1.0 + 1.1 + 1.2** forwardeados completos: instancia, physical devices, device, queues, memoria, buffers, imágenes, samplers, descriptor sets, pipelines gráficos y compute, command buffers, submit, fences, semáforos (incluido timeline), events, query pools, renderpass2, draw indirect count, buffer device address.
- **Swapchain emulada** por `VK_KHR_swapchain` + `VK_KHR_xcb_surface`: el ICD crea imágenes offscreen, el daemon copia a un buffer vía transfer, el ICD dibuja en la ventana X con `xcb_put_image`.
- **Cadena pNext** para `GetPhysicalDeviceProperties2` y `GetPhysicalDeviceFeatures2`: las properties y features de 1.1 y 1.2 llegan completas, filtradas por máscara para lo que el ICD no puede cumplir.
- **Present con MIT-SHM opcional**: swapchain usa `memfd` + `xcb_shm_attach_fd` (SHM 1.2) cuando el X server lo soporta; si no, cae a `xcb_put_image`. Medido +24% a +69% sobre Mali-G52 MC2 (ver más abajo).
- **Replay de command buffers** sobre el device real del daemon.

Verificado con `probe_api`: **19/19 PASS** cubriendo instancia 1.2, cadenas pNext, `vkBindBufferMemory2` real, timeline semaphores con signal/wait, buffer device address, `vkGetDescriptorSetLayoutSupport`, `vkCreateRenderPass2`, `vkCreateComputePipelines`.

Verificado end-to-end con `vkcube`, `vkcubepp` y `vkmark` sobre X11.

## Rendimiento medido

**vkmark 2.x, `--winsys xcb`, `-p fifo`, `-d 15000` (15 segundos por escena), Mali-G52 MC2.**

Corrida en condiciones de sobrecarga mínima: sin otras apps abiertas, sin compilaciones en background, sin cambios de foco durante la ejecución.

### 800×600 — Score 90

| Escena | FPS | FrameTime |
|---|---:|---:|
| vertex (device-local=true) | 96 | 10.417 ms |
| vertex (device-local=false) | 96 | 10.417 ms |
| texture (anisotropy=0) | 101 | 9.901 ms |
| texture (anisotropy=16) | 95 | 10.526 ms |
| shading (gouraud) | 87 | 11.494 ms |
| shading (blinn-phong-inf) | 89 | 11.236 ms |
| shading (phong) | 81 | 12.346 ms |
| shading (cel) | 81 | 12.346 ms |
| effect2d (kernel=edge) | 83 | 12.048 ms |
| effect2d (kernel=blur) | 59 | 16.949 ms |
| desktop (default) | 89 | 11.236 ms |
| cube (default) | 107 | 9.346 ms |
| clear (default) | 108 | 9.259 ms |

### 1280×720 — Score 56

| Escena | FPS | FrameTime |
|---|---:|---:|
| vertex (device-local=true) | 60 | 16.667 ms |
| vertex (device-local=false) | 60 | 16.667 ms |
| texture (anisotropy=0) | 60 | 16.667 ms |
| texture (anisotropy=16) | 60 | 16.667 ms |
| shading (gouraud) | 59 | 16.949 ms |
| shading (blinn-phong-inf) | 59 | 16.949 ms |
| shading (phong) | 58 | 17.241 ms |
| shading (cel) | 59 | 16.949 ms |
| effect2d (kernel=edge) | 50 | 20.000 ms |
| effect2d (kernel=blur) | 34 | 29.412 ms |
| desktop (default) | 60 | 16.667 ms |
| cube (default) | 60 | 16.667 ms |
| clear (default) | 61 | 16.393 ms |

### 1600×720

La corrida abortó con `SIGABRT` (exit 134) a los 3 segundos. Pendiente de diagnóstico. Probablemente el `maxImageDimension` del swapchain, que a 1600 px de ancho excede algún límite del driver Mali para el path de transfer + readback que usa Spatha.

### Nota sobre los resultados

**Posible cap de frames del sistema.** Los 60.0 FPS consistentes en 1280×720 (con `FrameTime` de exactamente 16.667 ms en seis escenas distintas) sugieren un techo por sincronización vertical en algún punto del stack de presentación (Spatha + X11 + Termux:X11). A 800×600 el mismo benchmark alcanza 108 FPS, lo cual descarta un cap global y apunta a un límite dependiente de la resolución de la ventana o del modo de presentación elegido. Los números de 1280×720 deben leerse como **piso**, no como techo. Pendiente de investigar si es `eglSwapInterval(1)` en Scutum o la cadencia del `xcb_put_image` en Spatha.

## Present: SHM vs put_image

Dos caminos de present, el ICD elige uno automáticamente:

- **SHM** (default si el server soporta SHM 1.2): 2 segmentos `memfd_create` en ping-pong, attachados con `xcb_shm_attach_fd`. Cada present manda ~32 bytes de header; el server lee los píxeles directo del mmap.
- **put_image** (fallback): el ICD recibe el frame en `malloc`, lo trocea según `max_req_len` y lo manda en múltiples writes. Varios `memcpy` por frame, todos evitables.

`SPATHA_NOSHM=1` fuerza el fallback.

### Números medidos (SHM vs NOSHM)

Mali-G52 MC2 / Termux:X11, `vkmark --winsys xcb -p immediate`, 3 s cube + 5 s effect2d, una corrida por celda.

| Resolución | Modo  | cube FPS | effect2d FPS | Score |
|------------|-------|---------:|-------------:|------:|
| 640×360    | SHM   |      104 |           63 |    83 |
| 640×360    | NOSHM |       61 |           58 |    59 |
| 854×480    | SHM   |       75 |           59 |    67 |
| 854×480    | NOSHM |       58 |           51 |    54 |
| 1280×720   | SHM   |       59 |           38 |    48 |
| 1280×720   | NOSHM |       46 |           24 |    35 |
| 1920×1080  | SHM   |       33 |           21 |    27 |
| 1920×1080  | NOSHM |       20 |           12 |    16 |

Delta del score: **+41% / +24% / +37% / +69%** (360p / 480p / 720p / 1080p). La ganancia escala con la resolución, consistente con el modelo: el overhead de NOSHM crece lineal con los píxeles, SHM queda en ~32 bytes fijos por frame.

Con `vkcube --c 600` a 800×600: SHM ~101 FPS vs NOSHM ~83 FPS (+22%).

### Límite conocido: 4K

`SPATHA_MAX_PAYLOAD` en `proto.h` es 16 MB. Un frame 3840×2160 RGBA8 son ~31.6 MB, así que el daemon rechaza el present con `VK_ERROR_OUT_OF_HOST_MEMORY`. 1920×1080 (8.3 MB) entra sin problema. Subir el límite es trivial pero cuadruplica los buffers de wire.

## En qué está testeado

| Componente | Valor |
|---|---|
| GPU | Mali-G52 MC2 (vendorID 0x13b5, deviceID 0x74021000) |
| Driver | ARM propietario, reporta Vulkan 1.3.278 |
| Host | Android bionic, `/system/lib64/libvulkan.so` |
| Cliente | Ubuntu glibc aarch64 dentro de proot-distro |
| Entorno | Gladiator (host) + Termux:X11 (display) |
| ICD negotiated | interface 7, expone hasta Vulkan 1.2 |

## Qué no soporta

- **Vulkan 1.3.** El driver Mali reporta 1.3.278 pero el ICD no forwardea ninguna función de 1.3. `GetPhysicalDeviceProperties2` capa la `apiVersion` reportada a 1.2.
- **Sparse binding y sparse residency.** Máscara apaga todos los `sparse*` en `VkPhysicalDeviceFeatures`.
- **YCbCr conversion y protected memory.**
- **Imageless framebuffer, separate depth-stencil layouts, bufferDeviceAddressCaptureReplay, bufferDeviceAddressMultiDevice.**
- **Swapchain nativa.** No usa el WSI de Mali. No funciona con `VK_KHR_display`, Wayland, ni compositores que esperen `VK_KHR_present_*`.
- **External memory / external semaphores / external fences.** Devuelven 0 handle types; no hay interoperabilidad real con Android hardware buffers.
- **Present mode Mailbox.** Solo FIFO e IMMEDIATE. `vkmark` con `-p mailbox` o `-p triple` falla con `VK_ERROR_FEATURE_NOT_PRESENT`.

## Dónde no se recomienda

- **Cualquier GPU que no sea Mali-G52 MC2.** Adreno, PowerVR, Mali-G77, Mali-G710, Xclipse, RDNA — todas sin testear. El código es genérico pero cada driver tiene sus rarezas.
- **Cargas throughput-intensivas.** Cada llamada Vulkan cruza un socket Unix con serialización y copia. Para uploads de texturas grandes, submit con cientos de dependencias, o compute de alta frecuencia hay una penalización medible.
- **Apps que requieran Vulkan 1.3 explícitamente** (Zink reciente, DXVK reciente, VKD3D-Proton reciente). Fallarán con `VK_ERROR_FEATURE_NOT_PRESENT`.
- **Apps que usen present nativo** (Wayland, KMS). Solo funciona con X11/XCB por readback.
- **Entornos sin Termux o sin un container glibc armado.**

## Limitaciones conocidas

- **Zink no carga en Android.** Mesa necesita DRM/GBM (`/dev/dri/*`) que Android no expone para Mali. Cae a drisw y aborta. No es limitación del ICD — el stack Vulkan funciona perfecto por debajo.
- **Memoria mapeada hace copia local.** `vkMapMemory` aloca un shadow buffer; los cambios se sincronizan al daemon en submit/unmap/flush.
- **`xcb_put_image` en el fallback.** Una copia extra por frame cuando SHM no está disponible.
- **Comandos de command buffer en streaming, no batcheados.**

## Arquitectura

    app glibc (vkcube, vkmark, app real)
      -> libspatha-icd.so       (ICD stub glibc)
      -> socket Unix            (SPATHA_SOCK)
      -> spathad                (daemon bionic)
      -> /system/lib64/libvulkan.so (loader Android)
      -> /vendor/lib64/hw/vulkan.mali.so (driver Mali)
      -> GPU

## Estructura

    glibc/    ICD stub (container, gcc)
    bionic/   daemon (host Android, clang)
    build/    binarios compilados

## Compilar

Daemon (Termux, bionic):

    cd bionic && make

ICD (container glibc):

    cd glibc && make

Los binarios salen en `build/bionic/spathad` y `build/glibc/libspatha-icd.so`.

## Instalación

Binarios del release v1.2:

- `libspatha-icd.so` → `/usr/lib/aarch64-linux-gnu/` del container
- `spatha_icd.json` → `/usr/share/vulkan/icd.d/` del container
- `spathad` → `$PREFIX/bin/` del host

Arrancar el daemon (Termux):

    SPATHA_SOCK=$PREFIX/tmp/spatha.sock spathad

Variables de entorno del cliente:

    SPATHA_SOCK=/host-tmp/spatha.sock
    VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/spatha_icd.json

## Variables

- `SPATHA_SOCK` — path al socket. Default `/tmp/spatha.sock`.
- `SPATHA_ICD_DAEMON` — `off` | `try` | `require`. Default `require`.
- `SPATHA_DEBUG=1` — imprime trazas a stderr.
- `SPATHA_NOSHM=1` — desactiva MIT-SHM y fuerza `xcb_put_image`.
- `SPATHA_VK_LIB` (solo daemon) — loader Android. Default `/system/lib64/libvulkan.so`.

## Archivos

- `libspatha-icd.c` — ICD stub glibc
- `spathad.c` — daemon bionic
- `proto.h` / `proto.c` — protocolo binario (framing + opcodes)
- `wire.h` — serialización
- `chain.h` — tablas pNext (1.1/1.2) + máscaras
- `icd_v12.inc` — lado ICD de la capa 1.1/1.2
- `spathad_v12.inc` — lado daemon de la capa 1.1/1.2
- `spathad_cmds.inc` — casos extra del replay
- `probe_api.c` — test de regresión 1.0+1.1+1.2
- `spatha_icd.json` — manifest del ICD

## Licencia

MIT. Ver LICENSE.
