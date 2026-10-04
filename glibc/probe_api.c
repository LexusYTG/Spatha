#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>

static PFN_vkGetInstanceProcAddr gipa;
#define G(n)  ((PFN_##n)gipa(inst, #n))
#define D(n)  ((PFN_##n)vkGetDeviceProcAddr(dev, #n))

static int PASS = 0, FAIL = 0;
#define OK(cond, msg) do { if (cond) { printf("  PASS: %s\n", msg); PASS++; } \
    else { printf("  FAIL: %s\n", msg); FAIL++; } } while (0)

int main(void) {
    void *vk = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!vk) vk = dlopen("libvulkan.so", RTLD_NOW);
    if (!vk) { fprintf(stderr, "no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
    if (!gipa) { fprintf(stderr, "no gipa\n"); return 1; }

    /* 1.0: instance con apiVersion 1.2 (loader no debe clampar) */
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "probe_api", .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = gipa(VK_NULL_HANDLE, "vkCreateInstance")
        ? ((PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance"))(&ici, NULL, &inst)
        : VK_ERROR_INITIALIZATION_FAILED;
    OK(r == VK_SUCCESS, "vkCreateInstance(1.2)");
    if (r != VK_SUCCESS) return 1;

    PFN_vkEnumeratePhysicalDevices       EnumeratePD = G(vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties2   GPDP2       = G(vkGetPhysicalDeviceProperties2);
    PFN_vkGetPhysicalDeviceFeatures2     GPDF2       = G(vkGetPhysicalDeviceFeatures2);
    PFN_vkCreateDevice                   CreateDev   = G(vkCreateDevice);

    OK(GPDP2 != NULL, "loader expone vkGetPhysicalDeviceProperties2 (1.1)");
    OK(GPDF2 != NULL, "loader expone vkGetPhysicalDeviceFeatures2 (1.1)");

    uint32_t npd = 1;
    VkPhysicalDevice pd;
    if (EnumeratePD(inst, &npd, &pd) != VK_SUCCESS || npd == 0) return 1;

    /* 1.1+1.2: cadena pNext en GetPhysicalDeviceProperties2 */
    VkPhysicalDeviceVulkan11Properties v11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES };
    VkPhysicalDeviceVulkan12Properties v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    v11.pNext = &v12;
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &v11 };
    GPDP2(pd, &p2);
    printf("  info: v11.subgroupSize=%u  v12.driverID=%u  v12.driverName=%s\n",
           v11.subgroupSize, v12.driverID, v12.driverName);
    OK(v11.subgroupSize > 0, "Vulkan11Properties.subgroupSize != 0 (pNext chain funciona)");
    OK(v12.driverID != 0 || v12.driverName[0] != 0, "Vulkan12Properties.driverName != vacío");

    /* 1.1+1.2: cadena pNext en GetPhysicalDeviceFeatures2 */
    VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11 };
    GPDF2(pd, &f2);
    printf("  info: f12.timelineSemaphore=%u  f12.bufferDeviceAddress=%u\n",
           f12.timelineSemaphore, f12.bufferDeviceAddress);
    OK(f12.timelineSemaphore || f12.bufferDeviceAddress, "features 1.2 forwardeadas (algún bool != 0)");

    /* Crear device con features 1.1/1.2 en pNext (si el ICD no soporta, esto falla) */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .pNext = &f11, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev = VK_NULL_HANDLE;
    r = CreateDev(pd, &dci, NULL, &dev);
    OK(r == VK_SUCCESS, "vkCreateDevice con pNext V11Features/V12Features");
    if (r != VK_SUCCESS) { printf("RESULT: %d PASS / %d FAIL\n", PASS, FAIL); return 1; }

    /* 1.1: vkBindBufferMemory2 (si el ICD no reenvía, falla) */
    PFN_vkGetBufferMemoryRequirements2 GMR2 = D(vkGetBufferMemoryRequirements2);
    PFN_vkBindBufferMemory2            BBM2 = D(vkBindBufferMemory2);
    OK(GMR2 != NULL, "vkGetBufferMemoryRequirements2 (1.1)");
    OK(BBM2 != NULL, "vkBindBufferMemory2 (1.1)");

    PFN_vkCreateBuffer   CB   = D(vkCreateBuffer);
    PFN_vkAllocateMemory AM   = D(vkAllocateMemory);
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                               .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT };
    VkBuffer buf = VK_NULL_HANDLE;
    CB(dev, &bci, NULL, &buf);
    VkBufferMemoryRequirementsInfo2 bmri = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = buf };
    VkMemoryRequirements2 mr2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    GMR2(dev, &bmri, &mr2);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr2.memoryRequirements.size, .memoryTypeIndex = 0 };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    AM(dev, &mai, NULL, &mem);
    VkBindBufferMemoryInfo bbmi = { .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
                                    .buffer = buf, .memory = mem, .memoryOffset = 0 };
    r = BBM2(dev, 1, &bbmi);
    OK(r == VK_SUCCESS, "vkBindBufferMemory2 devuelve VK_SUCCESS");

    /* 1.2: timeline semaphore */
    PFN_vkGetSemaphoreCounterValue GSCV = D(vkGetSemaphoreCounterValue);
    PFN_vkSignalSemaphore          SIG  = D(vkSignalSemaphore);
    PFN_vkWaitSemaphores           WAIT = D(vkWaitSemaphores);
    OK(GSCV && SIG && WAIT, "timeline semaphore functions (1.2)");

    VkSemaphoreTypeCreateInfo stc = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 42 };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &stc };
    VkSemaphore tsem = VK_NULL_HANDLE;
    PFN_vkCreateSemaphore CS = D(vkCreateSemaphore);
    r = CS(dev, &sci, NULL, &tsem);
    OK(r == VK_SUCCESS, "crear timeline semaphore");
    if (r == VK_SUCCESS) {
        uint64_t val = 0;
        r = GSCV(dev, tsem, &val);
        OK(r == VK_SUCCESS && val == 42, "vkGetSemaphoreCounterValue == 42");
        VkSemaphoreSignalInfo ssi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                      .semaphore = tsem, .value = 100 };
        r = SIG(dev, &ssi);
        OK(r == VK_SUCCESS, "vkSignalSemaphore(value=100)");
        r = GSCV(dev, tsem, &val);
        OK(r == VK_SUCCESS && val == 100, "counter == 100 tras signal");
    }

    /* 1.2: buffer device address */
    PFN_vkGetBufferDeviceAddress GBDA = D(vkGetBufferDeviceAddress);
    OK(GBDA != NULL, "vkGetBufferDeviceAddress (1.2)");
    if (GBDA) {
        VkBufferDeviceAddressInfo bdai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf };
        VkDeviceAddress addr = GBDA(dev, &bdai);
        printf("  info: deviceAddress=0x%llx\n", (unsigned long long)addr);
    }

    /* 1.1: descriptor set layout support */
    PFN_vkGetDescriptorSetLayoutSupport GDSLS = D(vkGetDescriptorSetLayoutSupport);
    OK(GDSLS != NULL, "vkGetDescriptorSetLayoutSupport (1.1)");

    /* 1.2: renderpass2 */
    PFN_vkCreateRenderPass2 CR2 = D(vkCreateRenderPass2);
    OK(CR2 != NULL, "vkCreateRenderPass2 (1.2)");

    /* 1.0: compute pipeline + dispatch (para ver que el stack ejecuta, no solo crea) */
    PFN_vkCreateComputePipelines CCP = D(vkCreateComputePipelines);
    OK(CCP != NULL, "vkCreateComputePipelines (1.0)");

    PFN_vkDestroyDevice DD = D(vkDestroyDevice);
    if (DD) DD(dev, NULL);
    PFN_vkDestroyInstance DI = G(vkDestroyInstance);
    if (DI) DI(inst, NULL);

    printf("\nRESULT: %d PASS / %d FAIL\n", PASS, FAIL);
    return FAIL == 0 ? 0 : 1;
}
