<div align="center">

> *"Mali dux, Mali bonum, Mali vita est."*  
> ***Ave, Mali! Ave, ARM!***  
❤️‍🔥

</div>

# Spatha

Puente Vulkan sin root entre Android y containers glibc.

## Qué es

Dos binarios que hablan por un socket Unix:

- **spathad** (bionic, corre en el host Android) carga
  /system/lib64/libvulkan.so — el loader del sistema — que a su vez
  carga el driver de la GPU del dispositivo.
- **libspatha-icd.so** (glibc, corre en el container) se registra como
  ICD de Vulkan. Cada entry point que la app invoca se serializa al
  socket, el daemon lo ejecuta contra el driver real y devuelve el
  resultado.

El container ve la GPU nativa del dispositivo como si fuera suya, sin
root, sin /dev/dri, sin tocar /vendor.

## Qué pretende

Dar acceso a aceleración Vulkan real a entornos glibc embebidos
(proot-distro, Termux, chroot) que de otra forma quedan atados a
llvmpipe o a emulación por software. Es el sustrato para correr apps
gráficas de escritorio en Android sin depender de que Mesa traiga un
backend DRM que Android no expone.

## Qué logra

- **Vulkan 1.0 + 1.1 + 1.2** forwardeados completos: instancia,
  physical devices, device, queues, memoria, buffers, imágenes,
  samplers, descriptor sets, pipelines gráficos y compute, command
  buffers, submit, fences, semáforos (incluido timeline), events,
  query pools, renderpass2, draw indirect count, buffer device
  address.
- **Swapchain emulada** por VK_KHR_swapchain + VK_KHR_xcb_surface:
  el ICD crea imágenes offscreen, el daemon copia a un buffer vía
  transfer, el ICD dibuja en la ventana X con xcb_put_image.
- **Cadena pNext** para GetPhysicalDeviceProperties2 y
  GetPhysicalDeviceFeatures2: las properties y features de 1.1 y 1.2
  llegan completas, filtradas por máscara para lo que el ICD no puede
  cumplir.
- **Replay de command buffers** sobre el device real del daemon.

Verificado con probe_api: **19/19 PASS** cubriendo instancia 1.2,
cadenas pNext, vkBindBufferMemory2 real, timeline semaphores con
signal/wait, buffer device address, vkGetDescriptorSetLayoutSupport,
vkCreateRenderPass2, vkCreateComputePipelines.

También verificado con vkcube y vkcubepp end-to-end.

## En qué está testeado

| Componente | Valor |
|---|---|
| GPU | Mali-G52 MC2 (vendorID 0x13b5, deviceID 0x74021000) |
| Driver | ARM propietario, reporta Vulkan 1.3.278 |
| Host | Android bionic, /system/lib64/libvulkan.so |
| Cliente | Ubuntu glibc aarch64 dentro de proot-distro |
| Entorno | Termux (host) + Termux:X11 (display) |
| ICD negotiated | interface 7, expone hasta Vulkan 1.2 |

Nada más está testeado.

## Qué no soporta

- **Vulkan 1.3.** El driver Mali reporta 1.3.278 pero el ICD no
  forwardea ninguna función de 1.3. GetPhysicalDeviceProperties2
  capa la apiVersion reportada a 1.2.
- **Sparse binding y sparse residency.** Máscara apaga todos los
  sparse* en VkPhysicalDeviceFeatures.
- **YCbCr conversion y protected memory.**
- **Imageless framebuffer, separate depth-stencil layouts,
  bufferDeviceAddressCaptureReplay, bufferDeviceAddressMultiDevice.**
- **Swapchain nativa.** No usa el WSI de Mali. No funciona con
  VK_KHR_display, Wayland, ni compositores que esperen
  VK_KHR_present_*.
- **External memory / external semaphores / external fences.**
  Devuelven 0 handle types; no hay interoperabilidad real con Android
  hardware buffers.

## Dónde no se recomienda

- **Cualquier GPU que no sea Mali-G52 MC2.** Adreno, PowerVR,
  Mali-G77, Mali-G710, Xclipse, RDNA — todas sin testear. El código
  es genérico pero cada driver tiene sus rarezas.
- **Cargas throughput-intensivas.** Cada llamada Vulkan cruza un
  socket Unix con serialización y copia. Para uploads de texturas
  grandes, submit con cientos de dependencias, o compute de alta
  frecuencia hay una penalización medible.
- **Apps que requieran Vulkan 1.3 explícitamente** (Zink, DXVK
  reciente, VKD3D-Proton reciente). Fallarán con
  VK_ERROR_FEATURE_NOT_PRESENT.
- **Apps que usen present nativo** (Wayland, KMS). Solo funciona con
  X11/XCB por readback.
- **Entornos sin Termux o sin un container glibc armado.**

## Limitaciones conocidas

- **Zink no carga en Android.** Mesa necesita DRM/GBM (/dev/dri/*)
  que Android no expone para Mali. Cae a drisw y aborta. No es
  limitación del ICD — el stack Vulkan funciona perfecto por debajo.
- **Memoria mapeada hace copia local.** vkMapMemory aloca un shadow
  buffer; los cambios se sincronizan al daemon en
  submit/unmap/flush.
- **Present usa xcb_put_image, no MIT-SHM.** Una copia extra.
- **Comandos de command buffer en streaming, no batcheados.**

## Arquitectura

    app glibc (vkcube, vkmark, app real)
      -> libspatha-icd.so       (ICD stub glibc)
      -> socket Unix            (SPATHA_SOCK)
      -> spathad                (daemon bionic)
      -> /system/lib64/libvulkan.so (loader Android)
      -> /vendor/lib64/hw/vulkan.mali.so (driver Mali)
      -> GPU

## Archivos

- libspatha-icd.c     ICD stub glibc
- spathad.c           Daemon bionic
- proto.h / proto.c   Protocolo binario (framing + opcodes)
- wire.h              Serialización
- chain.h             Tablas pNext (1.1/1.2) + máscaras
- icd_v12.inc         Lado ICD de la capa 1.1/1.2
- spathad_v12.inc     Lado daemon de la capa 1.1/1.2
- spathad_cmds.inc    Casos extra del replay
- probe_api.c         Test de regresión 1.0+1.1+1.2
- spatha_icd.json     Manifest del ICD

## Uso

Compilar el daemon (Termux, bionic):

    clang -O2 -Wall -Wextra -pthread -o spathad spathad.c proto.c -ldl

Compilar el ICD (container glibc):

    gcc -O2 -Wall -fPIC -shared -pthread -o libspatha-icd.so libspatha-icd.c proto.c -ldl

Arrancar el daemon (Termux):

    SPATHA_SOCK=/data/data/com.termux/files/usr/tmp/spatha.sock ./spathad

Instalar el ICD en el container:

    cp libspatha-icd.so /usr/lib/aarch64-linux-gnu/
    cp spatha_icd.json /usr/share/vulkan/icd.d/

Variables de entorno del cliente:

    SPATHA_SOCK=/host-tmp/spatha.sock
    SPATHA_ICD_DAEMON=require    # off | try | require
    VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/spatha_icd.json

## Variables

- SPATHA_SOCK: path al socket. Default /tmp/spatha.sock.
- SPATHA_ICD_DAEMON: off | try | require. Default off.
- SPATHA_DEBUG: 1 imprime trazas a stderr.
- SPATHA_VK_LIB (solo daemon): loader Android. Default
  /system/lib64/libvulkan.so.

## Licencia

MIT. Ver LICENSE.
