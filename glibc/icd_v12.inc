static uint32_t spatha_api_version(const VkPhysicalDeviceProperties *host)
{
    uint32_t hv = host->apiVersion, hmaj = VK_API_VERSION_MAJOR(hv), hmin = VK_API_VERSION_MINOR(hv);
    if (hmaj > 1 || (hmaj == 1 && hmin >= 2))
        return VK_MAKE_API_VERSION(0, 1, 2, VK_API_VERSION_PATCH(hv));
    return VK_MAKE_API_VERSION(0, hmaj, hmin, VK_API_VERSION_PATCH(hv));
}

static uint32_t pd_minor(VkPhysicalDevice h)
{
    VkPhysicalDeviceProperties p;
    stub_vkGetPhysicalDeviceProperties(h, &p);
    return VK_API_VERSION_MINOR(p.apiVersion);
}

static void xw(struct wbuf *w, uint32_t type, const void *pnext)
{
    switch (type) {
    case SPATHA_T_SEM: {
        const VkSemaphoreTypeCreateInfo *t = spatha_chain_find(pnext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
        if (t) { w_u64(w, 1); w_u64(w, (uint64_t)t->semaphoreType); w_u64(w, t->initialValue); return; }
        break; }
    case SPATHA_T_DSL: {
        const VkDescriptorSetLayoutBindingFlagsCreateInfo *f =
            spatha_chain_find(pnext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
        if (f && f->pBindingFlags) {
            w_u64(w, 1); w_u64(w, f->bindingCount);
            for (uint32_t i = 0; i < f->bindingCount; i++) w_u64(w, f->pBindingFlags[i]);
            return;
        }
        break; }
    case SPATHA_T_SAMPLER: {
        const VkSamplerReductionModeCreateInfo *s = spatha_chain_find(pnext, VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO);
        if (s) { w_u64(w, 1); w_u64(w, (uint64_t)s->reductionMode); return; }
        break; }
    case SPATHA_T_VIEW: {
        const VkImageViewUsageCreateInfo *u = spatha_chain_find(pnext, VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO);
        if (u) { w_u64(w, 1); w_u64(w, u->usage); return; }
        break; }
    case SPATHA_T_IMG: {
        const VkImageStencilUsageCreateInfo *su = spatha_chain_find(pnext, VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO);
        const VkImageFormatListCreateInfo *fl = spatha_chain_find(pnext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
        if (fl && (!fl->viewFormatCount || !fl->pViewFormats)) fl = NULL;
        uint64_t mask = (su ? 1u : 0u) | (fl ? 2u : 0u);
        w_u64(w, mask);
        if (su) w_u64(w, su->stencilUsage);
        if (fl) { w_u64(w, fl->viewFormatCount); for (uint32_t i = 0; i < fl->viewFormatCount; i++) w_u64(w, (uint64_t)fl->pViewFormats[i]); }
        return; }
    case SPATHA_T_RPASS: {
        const VkRenderPassMultiviewCreateInfo *mv = spatha_chain_find(pnext, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO);
        const VkRenderPassInputAttachmentAspectCreateInfo *ia =
            spatha_chain_find(pnext, VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO);
        uint64_t mask = (mv ? 1u : 0u) | (ia ? 2u : 0u);
        w_u64(w, mask);
        if (mv) {
            uint32_t nm = mv->pViewMasks ? mv->subpassCount : 0;
            uint32_t no = mv->pViewOffsets ? mv->dependencyCount : 0;
            uint32_t nc = mv->pCorrelationMasks ? mv->correlationMaskCount : 0;
            w_u64(w, nm); for (uint32_t i = 0; i < nm; i++) w_u64(w, mv->pViewMasks[i]);
            w_u64(w, no); for (uint32_t i = 0; i < no; i++) w_u64(w, (uint64_t)(int64_t)mv->pViewOffsets[i]);
            w_u64(w, nc); for (uint32_t i = 0; i < nc; i++) w_u64(w, mv->pCorrelationMasks[i]);
        }
        if (ia) {
            w_u64(w, ia->aspectReferenceCount);
            w_raw(w, ia->pAspectReferences, (size_t)ia->aspectReferenceCount * sizeof(VkInputAttachmentAspectReference));
        }
        return; }
    default: break;
    }
    w_u64(w, 0);
}

static void pd_chain_fill(struct spatha_physical_device *pd, uint32_t kind, VkBaseOutStructure *head)
{
    uint64_t cnt = 0; size_t cap = 8;
    for (VkBaseOutStructure *s = head; s; s = s->pNext) {
        size_t sz = spatha_chain_size(s->sType);
        if (sz) { cnt++; cap += (sz + 7) & ~(size_t)7; }
    }
    if (!cnt || cnt > 64) return;
    struct wbuf w; w_init(&w);
    w_u64(&w, pd->remote_id); w_u64(&w, kind); w_u64(&w, cnt);
    for (VkBaseOutStructure *s = head; s; s = s->pNext)
        if (spatha_chain_size(s->sType)) w_u64(&w, (uint64_t)s->sType);
    uint8_t *resp = malloc(cap);
    if (!resp) { w_free(&w); return; }
    size_t rl = 0;
    VkResult vr = vcall(pd->inst, SPATHA_OP_GET_PD_CHAIN, &w, resp, cap, &rl);
    w_free(&w);
    struct rbuf r; r_init(&r, resp + 8, rl >= 8 ? rl - 8 : 0);
    for (VkBaseOutStructure *s = head; s; s = s->pNext) {
        size_t sz = spatha_chain_size(s->sType);
        if (!sz) continue;
        uint8_t *dst = (uint8_t *)s + SPATHA_CHAIN_HDR;
        if (vr == VK_SUCCESS) {
            const uint8_t *b = r_blob(&r, sz);
            if (!b) { memset(dst, 0, sz - SPATHA_CHAIN_HDR); continue; }
            memcpy(dst, b + SPATHA_CHAIN_HDR, sz - SPATHA_CHAIN_HDR);
            spatha_mask_chain_struct(s);
        } else {
            memset(dst, 0, sz - SPATHA_CHAIN_HDR);
        }
    }
    free(resp);
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *f)
{
    if (!f) return;
    stub_vkGetPhysicalDeviceFeatures(physicalDevice, &f->features);
    struct spatha_physical_device *pd = (struct spatha_physical_device *)physicalDevice;
    if (!pd || !pd->inst) return;
    pd_chain_fill(pd, 0, (VkBaseOutStructure *)f->pNext);
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *p)
{
    if (!p) return;
    stub_vkGetPhysicalDeviceProperties(physicalDevice, &p->properties);
    struct spatha_physical_device *pd = (struct spatha_physical_device *)physicalDevice;
    if (!pd || !pd->inst) return;
    pd_chain_fill(pd, 1, (VkBaseOutStructure *)p->pNext);
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties2 *p)
{
    if (!n) return;
    VkQueueFamilyProperties tmp[SPATHA_MAX_QF];
    uint32_t total = SPATHA_MAX_QF;
    stub_vkGetPhysicalDeviceQueueFamilyProperties(pd, &total, tmp);
    if (!p) { *n = total; return; }
    uint32_t k = *n < total ? *n : total;
    for (uint32_t i = 0; i < k; i++) p[i].queueFamilyProperties = tmp[i];
    *n = k;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *p)
{ if (p) stub_vkGetPhysicalDeviceMemoryProperties(pd, &p->memoryProperties); }

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties2 *p)
{ if (p) stub_vkGetPhysicalDeviceFormatProperties(pd, fmt, &p->formatProperties); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *i,
                                               VkImageFormatProperties2 *p)
{
    if (!i || !p) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const VkPhysicalDeviceExternalImageFormatInfo *ex =
        spatha_chain_find(i->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
    if (ex && ex->handleType) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (i->flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                    VK_IMAGE_CREATE_SPARSE_ALIASED_BIT | VK_IMAGE_CREATE_PROTECTED_BIT))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return stub_vkGetPhysicalDeviceImageFormatProperties(pd, i->format, i->type, i->tiling, i->usage,
                                                         i->flags, &p->imageFormatProperties);
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice pd, const VkPhysicalDeviceSparseImageFormatInfo2 *i,
                                                     uint32_t *n, VkSparseImageFormatProperties2 *p)
{ (void)pd; (void)i; (void)p; if (n) *n = 0; }

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice pd, const VkPhysicalDeviceExternalBufferInfo *i,
                                                 VkExternalBufferProperties *p)
{
    (void)pd; if (!p) return;
    memset(&p->externalMemoryProperties, 0, sizeof(p->externalMemoryProperties));
    if (i) p->externalMemoryProperties.compatibleHandleTypes = i->handleType;
}
static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice pd, const VkPhysicalDeviceExternalFenceInfo *i,
                                                VkExternalFenceProperties *p)
{
    (void)pd; if (!p) return;
    p->exportFromImportedHandleTypes = 0; p->externalFenceFeatures = 0;
    p->compatibleHandleTypes = i ? i->handleType : 0;
}
static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice pd, const VkPhysicalDeviceExternalSemaphoreInfo *i,
                                                    VkExternalSemaphoreProperties *p)
{
    (void)pd; if (!p) return;
    p->exportFromImportedHandleTypes = 0; p->externalSemaphoreFeatures = 0;
    p->compatibleHandleTypes = i ? i->handleType : 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t *n, VkPhysicalDeviceGroupProperties *p)
{
    if (!n) return VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDevice pds[SPATHA_MAX_PD];
    uint32_t m = SPATHA_MAX_PD;
    VkResult r = stub_vkEnumeratePhysicalDevices(instance, &m, pds);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
    if (!p) { *n = m; return VK_SUCCESS; }
    uint32_t k = *n < m ? *n : m;
    for (uint32_t i = 0; i < k; i++) {
        p[i].physicalDeviceCount = 1;
        memset(p[i].physicalDevices, 0, sizeof(p[i].physicalDevices));
        p[i].physicalDevices[0] = pds[i];
        p[i].subsetAllocation = VK_FALSE;
    }
    *n = k;
    return k < m ? VK_INCOMPLETE : VK_SUCCESS;
}

struct spatha_dext { const char *name; uint32_t ver; uint32_t min_minor; };
static const struct spatha_dext g_dexts[] = {
    { "VK_KHR_swapchain", 70, 0 },
    { "VK_KHR_maintenance1", 2, 1 }, { "VK_KHR_maintenance2", 1, 1 }, { "VK_KHR_maintenance3", 1, 1 },
    { "VK_KHR_bind_memory2", 1, 1 }, { "VK_KHR_get_memory_requirements2", 1, 1 },
    { "VK_KHR_dedicated_allocation", 3, 1 }, { "VK_KHR_descriptor_update_template", 1, 1 },
    { "VK_KHR_16bit_storage", 1, 1 }, { "VK_KHR_multiview", 1, 1 }, { "VK_KHR_variable_pointers", 1, 1 },
    { "VK_KHR_shader_draw_parameters", 1, 1 }, { "VK_KHR_storage_buffer_storage_class", 1, 1 },
    { "VK_KHR_relaxed_block_layout", 1, 1 },
    { "VK_KHR_create_renderpass2", 1, 2 }, { "VK_KHR_draw_indirect_count", 1, 2 },
    { "VK_KHR_timeline_semaphore", 2, 2 }, { "VK_KHR_buffer_device_address", 1, 2 },
    { "VK_KHR_8bit_storage", 1, 2 }, { "VK_KHR_shader_float16_int8", 1, 2 },
    { "VK_KHR_shader_atomic_int64", 1, 2 }, { "VK_KHR_driver_properties", 1, 2 },
    { "VK_KHR_sampler_mirror_clamp_to_edge", 3, 2 }, { "VK_KHR_uniform_buffer_standard_layout", 1, 2 },
    { "VK_KHR_shader_subgroup_extended_types", 1, 2 }, { "VK_KHR_shader_float_controls", 4, 2 },
    { "VK_KHR_spirv_1_4", 1, 2 }, { "VK_KHR_vulkan_memory_model", 3, 2 },
    { "VK_KHR_depth_stencil_resolve", 1, 2 }, { "VK_KHR_image_format_list", 1, 2 },
    { "VK_EXT_host_query_reset", 1, 2 }, { "VK_EXT_descriptor_indexing", 2, 2 },
    { "VK_EXT_scalar_block_layout", 1, 2 }, { "VK_EXT_sampler_filter_minmax", 2, 2 },
};
#define SPATHA_N_DEXTS (sizeof(g_dexts) / sizeof(g_dexts[0]))

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p)
{
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!n) return VK_INCOMPLETE;
    uint32_t minor = pd_minor(pd), total = 0, k = 0;
    for (size_t i = 0; i < SPATHA_N_DEXTS; i++) if (g_dexts[i].min_minor <= minor) total++;
    if (!p) { *n = total; return VK_SUCCESS; }
    for (size_t i = 0; i < SPATHA_N_DEXTS; i++) {
        if (g_dexts[i].min_minor > minor) continue;
        if (k == *n) { *n = k; return VK_INCOMPLETE; }
        memset(&p[k], 0, sizeof(p[k]));
        strncpy(p[k].extensionName, g_dexts[i].name, VK_MAX_EXTENSION_NAME_SIZE - 1);
        p[k].specVersion = g_dexts[i].ver;
        k++;
    }
    *n = k;
    return VK_SUCCESS;
}

static VkResult dev_features_write(struct wbuf *w, const VkDeviceCreateInfo *ci)
{
    const VkPhysicalDeviceFeatures2 *f2 = spatha_chain_find(ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    const VkPhysicalDeviceFeatures *src = ci->pEnabledFeatures ? ci->pEnabledFeatures : (f2 ? &f2->features : NULL);
    VkPhysicalDeviceFeatures feat; memset(&feat, 0, sizeof(feat));
    if (src) feat = *src;
    VkPhysicalDeviceFeatures m = feat; spatha_mask_feat10(&m);
    if (memcmp(&m, &feat, sizeof(feat))) return VK_ERROR_FEATURE_NOT_PRESENT;
    w_u64(w, src != NULL);
    w_raw(w, &feat, sizeof(feat));

    uint64_t n = 0;
    for (const VkBaseInStructure *s = (const VkBaseInStructure *)ci->pNext; s; s = s->pNext) {
        size_t sz = spatha_chain_size(s->sType);
        if (!sz || sz > 512) continue;
        uint8_t tmp[512]; memcpy(tmp, s, sz);
        spatha_mask_chain_struct(tmp);
        if (memcmp(tmp + SPATHA_CHAIN_HDR, (const uint8_t *)s + SPATHA_CHAIN_HDR, sz - SPATHA_CHAIN_HDR))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        n++;
    }
    w_u64(w, n);
    for (const VkBaseInStructure *s = (const VkBaseInStructure *)ci->pNext; s; s = s->pNext) {
        size_t sz = spatha_chain_size(s->sType);
        if (!sz || sz > 512) continue;
        w_u64(w, (uint64_t)s->sType); w_u64(w, sz); w_raw(w, s, sz);
    }
    return VK_SUCCESS;
}

static uint64_t mem_alloc_flags(const VkMemoryAllocateInfo *ai)
{
    const VkMemoryAllocateFlagsInfo *f = spatha_chain_find(ai->pNext, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
    return f ? (uint64_t)f->flags : 0;
}

static void fill_dedicated(void *next)
{
    for (VkBaseOutStructure *s = (VkBaseOutStructure *)next; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            VkMemoryDedicatedRequirements *d = (VkMemoryDedicatedRequirements *)s;
            d->prefersDedicatedAllocation = VK_FALSE; d->requiresDedicatedAllocation = VK_FALSE;
        }
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *mr)
{ stub_vkGetBufferMemoryRequirements(device, i->buffer, &mr->memoryRequirements); fill_dedicated(mr->pNext); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkGetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *mr)
{ stub_vkGetImageMemoryRequirements(device, i->image, &mr->memoryRequirements); fill_dedicated(mr->pNext); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkGetImageSparseMemoryRequirements(VkDevice d, VkImage i, uint32_t *n, VkSparseImageMemoryRequirements *p)
{ (void)d; (void)i; (void)p; if (n) *n = 0; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkGetImageSparseMemoryRequirements2(VkDevice d, const VkImageSparseMemoryRequirementsInfo2 *i, uint32_t *n,
                                         VkSparseImageMemoryRequirements2 *p)
{ (void)d; (void)i; (void)p; if (n) *n = 0; }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkBindBufferMemory2(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo *b)
{
    for (uint32_t i = 0; i < n; i++) {
        VkResult r = stub_vkBindBufferMemory(device, b[i].buffer, b[i].memory, b[i].memoryOffset);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkBindImageMemory2(VkDevice device, uint32_t n, const VkBindImageMemoryInfo *b)
{
    for (uint32_t i = 0; i < n; i++) {
        VkResult r = stub_vkBindImageMemory(device, b[i].image, b[i].memory, b[i].memoryOffset);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetDeviceMemoryCommitment(VkDevice d, VkDeviceMemory m, VkDeviceSize *c)
{ (void)d; (void)m; if (c) *c = 0; }

static VKAPI_ATTR VkDeviceAddress VKAPI_CALL
stub_vkGetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo *i)
{
    DEV(device);
    struct wbuf w; w_init(&w); w_u64(&w, dv->remote_id); w_u64(&w, ID(i->buffer));
    alignas(8) uint8_t resp[16]; size_t rl = 0;
    VkResult r = vcall(dv->inst, SPATHA_OP_GET_BUFFER_ADDR, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    uint64_t a = 0;
    if (r == VK_SUCCESS && rl >= 16) memcpy(&a, resp + 8, 8);
    return (VkDeviceAddress)a;
}
static VKAPI_ATTR uint64_t VKAPI_CALL
stub_vkGetBufferOpaqueCaptureAddress(VkDevice d, const VkBufferDeviceAddressInfo *i)
{ (void)d; (void)i; return 0; }
static VKAPI_ATTR uint64_t VKAPI_CALL
stub_vkGetDeviceMemoryOpaqueCaptureAddress(VkDevice d, const VkDeviceMemoryOpaqueCaptureAddressInfo *i)
{ (void)d; (void)i; return 0; }

static VkResult sem_rpc(struct spatha_device *d, struct wbuf *w, uint64_t *out)
{
    alignas(8) uint8_t resp[16]; size_t rl = 0;
    VkResult r = vcall(d->inst, SPATHA_OP_SEM_OP, w, resp, sizeof(resp), &rl);
    w_free(w);
    if (out) { *out = 0; if (rl >= 16) memcpy(out, resp + 8, 8); }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore s, uint64_t *v)
{
    DEV(device); struct wbuf w; w_init(&w);
    w_u64(&w, dv->remote_id); w_u64(&w, 0); w_u64(&w, ID(s));
    return sem_rpc(dv, &w, v);
}
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    DEV(device); struct wbuf w; w_init(&w);
    w_u64(&w, dv->remote_id); w_u64(&w, 1); w_u64(&w, wi->flags); w_u64(&w, timeout); w_u64(&w, wi->semaphoreCount);
    for (uint32_t i = 0; i < wi->semaphoreCount; i++) w_u64(&w, ID(wi->pSemaphores[i]));
    w_raw(&w, wi->pValues, (size_t)wi->semaphoreCount * 8);
    return sem_rpc(dv, &w, NULL);
}
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkSignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo *si)
{
    DEV(device); struct wbuf w; w_init(&w);
    w_u64(&w, dv->remote_id); w_u64(&w, 2); w_u64(&w, ID(si->semaphore)); w_u64(&w, si->value);
    return sem_rpc(dv, &w, NULL);
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkQueueSubmit(VkQueue queue, uint32_t n, const VkSubmitInfo *si, VkFence fence)
{
    struct spatha_queue *q = (struct spatha_queue *)queue;
    struct spatha_device *d = q->dev;
    dev_flush_all(d);
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, q->remote_id); w_u64(&w, ID(fence)); w_u64(&w, n);
    for (uint32_t i = 0; i < n; i++) {
        const VkSubmitInfo *s = &si[i];
        w_u64(&w, s->waitSemaphoreCount);
        for (uint32_t k = 0; k < s->waitSemaphoreCount; k++) w_u64(&w, ID(s->pWaitSemaphores[k]));
        w_raw(&w, s->pWaitDstStageMask, (size_t)s->waitSemaphoreCount * 4);
        w_u64(&w, s->commandBufferCount);
        for (uint32_t k = 0; k < s->commandBufferCount; k++)
            w_u64(&w, ((struct spatha_cmdbuf *)s->pCommandBuffers[k])->remote_id);
        w_u64(&w, s->signalSemaphoreCount);
        for (uint32_t k = 0; k < s->signalSemaphoreCount; k++) w_u64(&w, ID(s->pSignalSemaphores[k]));
        const VkTimelineSemaphoreSubmitInfo *tl =
            spatha_chain_find(s->pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
        w_u64(&w, tl != NULL);
        if (tl) {
            uint32_t nwv = tl->pWaitSemaphoreValues ? tl->waitSemaphoreValueCount : 0;
            uint32_t nsv = tl->pSignalSemaphoreValues ? tl->signalSemaphoreValueCount : 0;
            w_u64(&w, nwv); w_raw(&w, tl->pWaitSemaphoreValues, (size_t)nwv * 8);
            w_u64(&w, nsv); w_raw(&w, tl->pSignalSemaphoreValues, (size_t)nsv * 8);
        }
    }
    alignas(8) uint8_t resp[16]; size_t rl;
    VkResult r = vcall(d->inst, SPATHA_OP_QUEUE_SUBMIT, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateEvent(VkDevice d, const VkEventCreateInfo *ci, const VkAllocationCallbacks *a, VkEvent *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_EVENT); xw(&w, SPATHA_T_EVENT, NULL); w_u64(&w, ci->flags);
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkEvent, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyEvent(VkDevice d, VkEvent e, const VkAllocationCallbacks *a)
{ (void)a; if (e) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_EVENT, ID(e)); }
static VkResult event_op(struct spatha_device *d, uint32_t op, VkEvent e)
{
    struct wbuf w; w_init(&w); w_u64(&w, d->remote_id); w_u64(&w, op); w_u64(&w, ID(e));
    alignas(8) uint8_t resp[16]; size_t rl;
    VkResult r = vcall(d->inst, SPATHA_OP_EVENT_OP, &w, resp, sizeof(resp), &rl);
    w_free(&w); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL stub_vkGetEventStatus(VkDevice d, VkEvent e)
{ return event_op((struct spatha_device *)d, 0, e); }
static VKAPI_ATTR VkResult VKAPI_CALL stub_vkSetEvent(VkDevice d, VkEvent e)
{ return event_op((struct spatha_device *)d, 1, e); }
static VKAPI_ATTR VkResult VKAPI_CALL stub_vkResetEvent(VkDevice d, VkEvent e)
{ return event_op((struct spatha_device *)d, 2, e); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_QPOOL); xw(&w, SPATHA_T_QPOOL, NULL); w_raw(&w, ci, sizeof(*ci));
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkQueryPool, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyQueryPool(VkDevice d, VkQueryPool p, const VkAllocationCallbacks *a)
{ (void)a; if (p) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_QPOOL, ID(p)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkGetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count, size_t dataSize,
                           void *data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    DEV(device);
    if (dataSize > SPATHA_MAX_PAYLOAD - 128) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct wbuf w; w_init(&w);
    w_u64(&w, dv->remote_id); w_u64(&w, ID(pool)); w_u64(&w, first); w_u64(&w, count);
    w_u64(&w, dataSize); w_u64(&w, stride); w_u64(&w, flags);
    size_t cap = 8 + dataSize + 8;
    uint8_t *resp = malloc(cap);
    if (!resp) { w_free(&w); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    size_t rl = 0;
    VkResult vr = vcall(dv->inst, SPATHA_OP_GET_QUERY_RESULTS, &w, resp, cap, &rl);
    w_free(&w);
    if ((vr == VK_SUCCESS || vr == VK_NOT_READY) && rl >= 8 + dataSize) memcpy(data, resp + 8, dataSize);
    else if (vr == VK_SUCCESS || vr == VK_NOT_READY) vr = VK_ERROR_DEVICE_LOST;
    free(resp);
    return vr;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkResetQueryPool(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count)
{
    DEV(device); struct wbuf w; w_init(&w);
    w_u64(&w, dv->remote_id); w_u64(&w, ID(pool)); w_u64(&w, first); w_u64(&w, count);
    alignas(8) uint8_t resp[16]; size_t rl;
    vcall(dv->inst, SPATHA_OP_RESET_QUERY_POOL, &w, resp, sizeof(resp), &rl);
    w_free(&w);
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateBufferView(VkDevice d, const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *a, VkBufferView *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_BVIEW); xw(&w, SPATHA_T_BVIEW, NULL); w_raw(&w, ci, sizeof(*ci));
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkBufferView, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyBufferView(VkDevice d, VkBufferView v, const VkAllocationCallbacks *a)
{ (void)a; if (v) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_BVIEW, ID(v)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreatePipelineCache(VkDevice d, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineCache *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_PCACHE);
  if (ci->initialDataSize && ci->pInitialData) {
      w_u64(&w, 1); w_u64(&w, ci->initialDataSize); w_raw(&w, ci->pInitialData, ci->initialDataSize);
  } else w_u64(&w, 0);
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkPipelineCache, id); return r; }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkGetPipelineCacheData(VkDevice device, VkPipelineCache cache, size_t *size, void *data)
{
    DEV(device);
    size_t cap = data ? *size : 0;
    if (cap > SPATHA_MAX_PAYLOAD - 128) cap = SPATHA_MAX_PAYLOAD - 128;
    struct wbuf w; w_init(&w);
    w_u64(&w, dv->remote_id); w_u64(&w, 0); w_u64(&w, ID(cache)); w_u64(&w, cap); w_u64(&w, data != NULL);
    size_t rcap = 16 + cap + 8;
    uint8_t *resp = malloc(rcap);
    if (!resp) { w_free(&w); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    size_t rl = 0;
    VkResult vr = vcall(dv->inst, SPATHA_OP_PCACHE_OP, &w, resp, rcap, &rl);
    w_free(&w);
    if ((vr == VK_SUCCESS || vr == VK_INCOMPLETE) && rl >= 16) {
        uint64_t sz; memcpy(&sz, resp + 8, 8);
        if (data) {
            size_t k = sz < cap ? (size_t)sz : cap;
            if (rl >= 16 + k) memcpy(data, resp + 16, k);
            *size = k;
        } else *size = (size_t)sz;
    } else if (vr == VK_SUCCESS) vr = VK_ERROR_DEVICE_LOST;
    free(resp);
    return vr;
}
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkMergePipelineCaches(VkDevice device, VkPipelineCache dst, uint32_t n, const VkPipelineCache *src)
{
    DEV(device); struct wbuf w; w_init(&w);
    w_u64(&w, dv->remote_id); w_u64(&w, 1); w_u64(&w, ID(dst)); w_u64(&w, n);
    for (uint32_t i = 0; i < n; i++) w_u64(&w, ID(src[i]));
    alignas(8) uint8_t resp[16]; size_t rl;
    VkResult r = vcall(dv->inst, SPATHA_OP_PCACHE_OP, &w, resp, sizeof(resp), &rl);
    w_free(&w); return r;
}

static void w_stage(struct wbuf *w, const VkPipelineShaderStageCreateInfo *s)
{
    w_raw(w, s, sizeof(*s));
    uint64_t nl = strlen(s->pName);
    w_u64(w, nl); w_raw(w, s->pName, nl);
    if (s->pSpecializationInfo) {
        const VkSpecializationInfo *sp = s->pSpecializationInfo;
        w_u64(w, 1); w_raw(w, sp, sizeof(*sp));
        w_raw(w, sp->pMapEntries, (size_t)sp->mapEntryCount * sizeof(VkSpecializationMapEntry));
        w_raw(w, sp->pData, sp->dataSize);
    } else w_u64(w, 0);
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
        const VkComputePipelineCreateInfo *cis, const VkAllocationCallbacks *a, VkPipeline *out)
{
    (void)a; DEV(device);
    struct wbuf w; w_init(&w);
    w_u64(&w, dv->remote_id); w_u64(&w, ID(cache)); w_u64(&w, n);
    for (uint32_t k = 0; k < n; k++) { w_raw(&w, &cis[k], sizeof(cis[k])); w_stage(&w, &cis[k].stage); }
    for (uint32_t i = 0; i < n; i++) out[i] = VK_NULL_HANDLE;
    size_t cap = 8 + (size_t)n * 8 + 64;
    uint8_t *resp = malloc(cap);
    if (!resp) { w_free(&w); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    size_t rl = 0;
    VkResult vr = vcall(dv->inst, SPATHA_OP_CREATE_COMPUTE_PIPELINES, &w, resp, cap, &rl);
    w_free(&w);
    if (vr == VK_SUCCESS) {
        if (rl < 8 + (size_t)n * 8) vr = VK_ERROR_DEVICE_LOST;
        else for (uint32_t i = 0; i < n; i++) { uint64_t id; memcpy(&id, resp + 8 + i * 8, 8); out[i] = TOH(VkPipeline, id); }
    }
    free(resp);
    return vr;
}

static VkResult pool_reset(struct spatha_device *d, uint32_t kind, uint64_t id, uint64_t flags)
{
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, kind); w_u64(&w, id); w_u64(&w, flags);
    alignas(8) uint8_t resp[16]; size_t rl;
    VkResult r = vcall(d->inst, SPATHA_OP_RESET_POOL, &w, resp, sizeof(resp), &rl);
    w_free(&w); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f)
{ return pool_reset((struct spatha_device *)d, SPATHA_T_CMDPOOL, ID(p), f); }
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkResetDescriptorPool(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f)
{ return pool_reset((struct spatha_device *)d, SPATHA_T_DPOOL, ID(p), f); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkTrimCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolTrimFlags f)
{ (void)d; (void)p; (void)f; }

static void dsl_payload(struct wbuf *w, const VkDescriptorSetLayoutCreateInfo *ci)
{
    xw(w, SPATHA_T_DSL, ci->pNext);
    w_raw(w, ci, sizeof(*ci));
    w_raw(w, ci->pBindings, (size_t)ci->bindingCount * sizeof(VkDescriptorSetLayoutBinding));
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
        if (!b->pImmutableSamplers) continue;
        for (uint32_t k = 0; k < b->descriptorCount; k++) w_u64(w, ID(b->pImmutableSamplers[k]));
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_DSL); dsl_payload(&w, ci);
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkDescriptorSetLayout, id); return r; }

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetDescriptorSetLayoutSupport(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayoutSupport *sup)
{
    DEV(device);
    VkDescriptorSetVariableDescriptorCountLayoutSupport *vs = NULL;
    for (VkBaseOutStructure *s = (VkBaseOutStructure *)sup->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT)
            vs = (VkDescriptorSetVariableDescriptorCountLayoutSupport *)s;
    sup->supported = VK_FALSE;
    if (vs) vs->maxVariableDescriptorCount = 0;
    struct wbuf w; w_init(&w); w_u64(&w, dv->remote_id); dsl_payload(&w, ci);
    alignas(8) uint8_t resp[8 + 16]; size_t rl = 0;
    VkResult r = vcall(dv->inst, SPATHA_OP_DSL_SUPPORT, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    if (r == VK_SUCCESS && rl >= sizeof(resp)) {
        uint64_t s, mv; memcpy(&s, resp + 8, 8); memcpy(&mv, resp + 16, 8);
        sup->supported = s ? VK_TRUE : VK_FALSE;
        if (vs) vs->maxVariableDescriptorCount = (uint32_t)mv;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
    struct spatha_device *d = (struct spatha_device *)device;
    uint32_t n = ai->descriptorSetCount;
    const VkDescriptorSetVariableDescriptorCountAllocateInfo *vc =
        spatha_chain_find(ai->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO);
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, ID(ai->descriptorPool)); w_u64(&w, n);
    for (uint32_t i = 0; i < n; i++) w_u64(&w, ID(ai->pSetLayouts[i]));
    uint32_t nv = (vc && vc->pDescriptorCounts) ? vc->descriptorSetCount : 0;
    w_u64(&w, nv);
    for (uint32_t i = 0; i < nv; i++) w_u64(&w, vc->pDescriptorCounts[i]);
    uint8_t *resp = malloc(8 + (size_t)n * 8 + 64);
    if (!resp) { w_free(&w); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    size_t rl = 0;
    VkResult vr = vcall(d->inst, SPATHA_OP_ALLOC_DESC_SETS, &w, resp, 8 + (size_t)n * 8 + 64, &rl);
    w_free(&w);
    for (uint32_t i = 0; i < n; i++) out[i] = VK_NULL_HANDLE;
    if (vr == VK_SUCCESS) {
        if (rl < 8 + (size_t)n * 8) vr = VK_ERROR_DEVICE_LOST;
        else for (uint32_t i = 0; i < n; i++) { uint64_t id; memcpy(&id, resp + 8 + i * 8, 8); out[i] = TOH(VkDescriptorSet, id); }
    }
    free(resp);
    return vr;
}

static int desc_is_image(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
static int desc_is_buffer(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
static int desc_is_texel(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkUpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *ws,
                            uint32_t nc, const VkCopyDescriptorSet *cs)
{
    struct spatha_device *d = (struct spatha_device *)device;
    uint32_t keep = 0;
    for (uint32_t i = 0; i < nw; i++) {
        VkDescriptorType t = ws[i].descriptorType;
        if (desc_is_image(t) || desc_is_buffer(t) || desc_is_texel(t)) keep++;
    }
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, keep); w_u64(&w, nc);
    for (uint32_t i = 0; i < nw; i++) {
        const VkWriteDescriptorSet *x = &ws[i];
        if (desc_is_image(x->descriptorType)) {
            w_raw(&w, x, sizeof(*x));
            w_raw(&w, x->pImageInfo, (size_t)x->descriptorCount * sizeof(VkDescriptorImageInfo));
        } else if (desc_is_buffer(x->descriptorType)) {
            w_raw(&w, x, sizeof(*x));
            w_raw(&w, x->pBufferInfo, (size_t)x->descriptorCount * sizeof(VkDescriptorBufferInfo));
        } else if (desc_is_texel(x->descriptorType)) {
            w_raw(&w, x, sizeof(*x));
            for (uint32_t k = 0; k < x->descriptorCount; k++) w_u64(&w, ID(x->pTexelBufferView[k]));
        }
    }
    w_raw(&w, cs, (size_t)nc * sizeof(VkCopyDescriptorSet));
    alignas(8) uint8_t resp[16]; size_t rl;
    vcall(d->inst, SPATHA_OP_UPDATE_DESC_SETS, &w, resp, sizeof(resp), &rl);
    w_free(&w);
}

struct spatha_dut { uint32_t n; VkDescriptorUpdateTemplateEntry *e; };

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateDescriptorUpdateTemplate(VkDevice d, const VkDescriptorUpdateTemplateCreateInfo *ci,
                                      const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out)
{
    (void)d; (void)a;
    if (ci->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct spatha_dut *t = calloc(1, sizeof(*t));
    if (!t) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t n = ci->descriptorUpdateEntryCount;
    t->e = calloc(n ? n : 1, sizeof(*t->e));
    if (!t->e) { free(t); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    if (n) memcpy(t->e, ci->pDescriptorUpdateEntries, (size_t)n * sizeof(*t->e));
    t->n = n;
    *out = TOH(VkDescriptorUpdateTemplate, t);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyDescriptorUpdateTemplate(VkDevice d, VkDescriptorUpdateTemplate tp, const VkAllocationCallbacks *a)
{
    (void)d; (void)a;
    struct spatha_dut *t = (struct spatha_dut *)(uintptr_t)tp;
    if (t) { free(t->e); free(t); }
}
static VKAPI_ATTR void VKAPI_CALL
stub_vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tp, const void *data)
{
    struct spatha_dut *t = (struct spatha_dut *)(uintptr_t)tp;
    if (!t || !t->n || !data) return;
    size_t total = 0;
    for (uint32_t i = 0; i < t->n; i++) total += t->e[i].descriptorCount;
    VkWriteDescriptorSet *ws = calloc(t->n, sizeof(*ws));
    VkDescriptorImageInfo *ii = calloc(total ? total : 1, sizeof(*ii));
    VkDescriptorBufferInfo *bi = calloc(total ? total : 1, sizeof(*bi));
    VkBufferView *bv = calloc(total ? total : 1, sizeof(*bv));
    if (ws && ii && bi && bv) {
        size_t oi = 0, ob = 0, ov = 0;
        for (uint32_t i = 0; i < t->n; i++) {
            const VkDescriptorUpdateTemplateEntry *e = &t->e[i];
            VkWriteDescriptorSet *x = &ws[i];
            x->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            x->dstSet = set; x->dstBinding = e->dstBinding; x->dstArrayElement = e->dstArrayElement;
            x->descriptorCount = e->descriptorCount; x->descriptorType = e->descriptorType;
            size_t si = oi, sb = ob, sv = ov;
            for (uint32_t k = 0; k < e->descriptorCount; k++) {
                const uint8_t *src = (const uint8_t *)data + e->offset + (size_t)k * e->stride;
                if (desc_is_image(e->descriptorType)) memcpy(&ii[oi++], src, sizeof(VkDescriptorImageInfo));
                else if (desc_is_buffer(e->descriptorType)) memcpy(&bi[ob++], src, sizeof(VkDescriptorBufferInfo));
                else memcpy(&bv[ov++], src, sizeof(VkBufferView));
            }
            if (desc_is_image(e->descriptorType)) x->pImageInfo = &ii[si];
            else if (desc_is_buffer(e->descriptorType)) x->pBufferInfo = &bi[sb];
            else x->pTexelBufferView = &bv[sv];
        }
        stub_vkUpdateDescriptorSets(device, t->n, ws, 0, NULL);
    }
    free(ws); free(ii); free(bi); free(bv);
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *out)
{
    (void)a; DEV(device);
    struct wbuf w; w_init(&w);
    w_u64(&w, dv->remote_id);
    w_raw(&w, ci, sizeof(*ci));
    w_raw(&w, ci->pAttachments, (size_t)ci->attachmentCount * sizeof(VkAttachmentDescription2));
    for (uint32_t i = 0; i < ci->subpassCount; i++) {
        const VkSubpassDescription2 *s = &ci->pSubpasses[i];
        w_raw(&w, s, sizeof(*s));
        w_raw(&w, s->pInputAttachments, (size_t)s->inputAttachmentCount * sizeof(VkAttachmentReference2));
        w_raw(&w, s->pColorAttachments, (size_t)s->colorAttachmentCount * sizeof(VkAttachmentReference2));
        if (s->pResolveAttachments) w_raw(&w, s->pResolveAttachments, (size_t)s->colorAttachmentCount * sizeof(VkAttachmentReference2));
        if (s->pDepthStencilAttachment) w_raw(&w, s->pDepthStencilAttachment, sizeof(VkAttachmentReference2));
        w_raw(&w, s->pPreserveAttachments, (size_t)s->preserveAttachmentCount * 4);
        const VkSubpassDescriptionDepthStencilResolve *dr =
            spatha_chain_find(s->pNext, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);
        if (dr && dr->pDepthStencilResolveAttachment) {
            w_u64(&w, 1); w_raw(&w, dr, sizeof(*dr));
            w_raw(&w, dr->pDepthStencilResolveAttachment, sizeof(VkAttachmentReference2));
        } else w_u64(&w, 0);
    }
    w_raw(&w, ci->pDependencies, (size_t)ci->dependencyCount * sizeof(VkSubpassDependency2));
    w_raw(&w, ci->pCorrelatedViewMasks, (size_t)ci->correlatedViewMaskCount * 4);
    alignas(8) uint8_t resp[32]; size_t rl = 0;
    VkResult vr = vcall(dv->inst, SPATHA_OP_CREATE_RENDERPASS2, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    if (vr != VK_SUCCESS) return vr;
    if (rl < 16) return VK_ERROR_DEVICE_LOST;
    uint64_t id; memcpy(&id, resp + 8, 8);
    *out = TOH(VkRenderPass, id);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    struct spatha_cmdbuf *c = (struct spatha_cmdbuf *)cb;
    w_free(&c->rec);
    c->flags = bi ? bi->flags : 0;
    c->recording = 1;
    c->has_inh = 0;
    if (bi && bi->pInheritanceInfo && c->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
        c->inh = *bi->pInheritanceInfo; c->inh.pNext = NULL; c->has_inh = 1;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL stub_vkEndCommandBuffer(VkCommandBuffer cb)
{
    struct spatha_cmdbuf *c = (struct spatha_cmdbuf *)cb;
    if (!c->recording) return VK_ERROR_UNKNOWN;
    c->recording = 0;
    if (c->rec.err) { w_free(&c->rec); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    struct wbuf w; w_init(&w);
    w_u64(&w, c->dev->remote_id); w_u64(&w, c->remote_id); w_u64(&w, c->flags); w_u64(&w, c->rec.len);
    w_raw(&w, c->rec.p, c->rec.len);
    w_u64(&w, c->has_inh);
    if (c->has_inh) w_raw(&w, &c->inh, sizeof(c->inh));
    alignas(8) uint8_t resp[16]; size_t rl;
    VkResult r = vcall(c->dev->inst, SPATHA_OP_CMDBUF_REPLAY, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    w_free(&c->rec);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *sbi)
{ stub_vkCmdBeginRenderPass(cb, bi, sbi ? sbi->contents : VK_SUBPASS_CONTENTS_INLINE); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdNextSubpass2(VkCommandBuffer cb, const VkSubpassBeginInfo *sbi, const VkSubpassEndInfo *sei)
{ (void)sei; stub_vkCmdNextSubpass(cb, sbi ? sbi->contents : VK_SUBPASS_CONTENTS_INLINE); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo *sei)
{ (void)sei; stub_vkCmdEndRenderPass(cb); }

static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z)
{ CMD(cb); w_u64(&b, x); w_u64(&b, y); w_u64(&b, z); CMD_DONE(SPATHA_CMD_DISPATCH); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off)
{ CMD(cb); w_u64(&b, ID(buf)); w_u64(&b, off); CMD_DONE(SPATHA_CMD_DISPATCH_INDIRECT); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdDispatchBase(VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y, uint32_t z)
{ CMD(cb); w_u64(&b, bx); w_u64(&b, by); w_u64(&b, bz); w_u64(&b, x); w_u64(&b, y); w_u64(&b, z);
  CMD_DONE(SPATHA_CMD_DISPATCH_BASE); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdSetDeviceMask(VkCommandBuffer cb, uint32_t mask) { (void)cb; (void)mask; }

static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, uint32_t count, uint32_t stride)
{ CMD(cb); w_u64(&b, ID(buf)); w_u64(&b, off); w_u64(&b, count); w_u64(&b, stride); CMD_DONE(SPATHA_CMD_DRAW_INDIRECT); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, uint32_t count, uint32_t stride)
{ CMD(cb); w_u64(&b, ID(buf)); w_u64(&b, off); w_u64(&b, count); w_u64(&b, stride); CMD_DONE(SPATHA_CMD_DRAW_INDEXED_INDIRECT); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkBuffer cbuf, VkDeviceSize coff,
                            uint32_t maxc, uint32_t stride)
{ CMD(cb); w_u64(&b, ID(buf)); w_u64(&b, off); w_u64(&b, ID(cbuf)); w_u64(&b, coff); w_u64(&b, maxc); w_u64(&b, stride);
  CMD_DONE(SPATHA_CMD_DRAW_INDIRECT_COUNT); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkBuffer cbuf, VkDeviceSize coff,
                                   uint32_t maxc, uint32_t stride)
{ CMD(cb); w_u64(&b, ID(buf)); w_u64(&b, off); w_u64(&b, ID(cbuf)); w_u64(&b, coff); w_u64(&b, maxc); w_u64(&b, stride);
  CMD_DONE(SPATHA_CMD_DRAW_INDEXED_INDIRECT_COUNT); }

static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearDepthStencilValue *v,
                                 uint32_t n, const VkImageSubresourceRange *r)
{ CMD(cb); w_u64(&b, ID(im)); w_u64(&b, l); w_raw(&b, v, sizeof(*v)); w_u64(&b, n);
  w_raw(&b, r, (size_t)n * sizeof(*r)); CMD_DONE(SPATHA_CMD_CLEAR_DEPTH_STENCIL); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *at, uint32_t nr, const VkClearRect *rc)
{ CMD(cb); w_u64(&b, na); w_raw(&b, at, (size_t)na * sizeof(*at)); w_u64(&b, nr); w_raw(&b, rc, (size_t)nr * sizeof(*rc));
  CMD_DONE(SPATHA_CMD_CLEAR_ATTACHMENTS); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdResolveImage(VkCommandBuffer cb, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl, uint32_t n, const VkImageResolve *r)
{ CMD(cb); w_u64(&b, ID(si)); w_u64(&b, sl); w_u64(&b, ID(di)); w_u64(&b, dl); w_u64(&b, n);
  w_raw(&b, r, (size_t)n * sizeof(*r)); CMD_DONE(SPATHA_CMD_RESOLVE_IMAGE); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize sz, const void *data)
{ CMD(cb); w_u64(&b, ID(buf)); w_u64(&b, off); w_u64(&b, sz); w_raw(&b, data, (size_t)sz); CMD_DONE(SPATHA_CMD_UPDATE_BUFFER); }

static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdSetDepthBounds(VkCommandBuffer cb, float mn, float mx)
{ CMD(cb); float f[2] = { mn, mx }; w_raw(&b, f, 8); CMD_DONE(SPATHA_CMD_SET_DEPTH_BOUNDS); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v)
{ CMD(cb); w_u64(&b, f); w_u64(&b, v); CMD_DONE(SPATHA_CMD_SET_STENCIL_COMPARE); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v)
{ CMD(cb); w_u64(&b, f); w_u64(&b, v); CMD_DONE(SPATHA_CMD_SET_STENCIL_WRITE); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v)
{ CMD(cb); w_u64(&b, f); w_u64(&b, v); CMD_DONE(SPATHA_CMD_SET_STENCIL_REF); }

static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdSetEvent(VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags m)
{ CMD(cb); w_u64(&b, ID(e)); w_u64(&b, m); CMD_DONE(SPATHA_CMD_SET_EVENT); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdResetEvent(VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags m)
{ CMD(cb); w_u64(&b, ID(e)); w_u64(&b, m); CMD_DONE(SPATHA_CMD_RESET_EVENT); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdWaitEvents(VkCommandBuffer cb, uint32_t ne, const VkEvent *ev, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
        uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb,
        uint32_t ni, const VkImageMemoryBarrier *ib)
{ CMD(cb); w_u64(&b, ne); for (uint32_t i = 0; i < ne; i++) w_u64(&b, ID(ev[i]));
  w_u64(&b, ss); w_u64(&b, ds);
  w_u64(&b, nm); w_raw(&b, mb, (size_t)nm * sizeof(*mb));
  w_u64(&b, nb); w_raw(&b, bb, (size_t)nb * sizeof(*bb));
  w_u64(&b, ni); w_raw(&b, ib, (size_t)ni * sizeof(*ib)); CMD_DONE(SPATHA_CMD_WAIT_EVENTS); }

static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *cbs)
{ CMD(cb); w_u64(&b, n);
  for (uint32_t i = 0; i < n; i++) w_u64(&b, ((struct spatha_cmdbuf *)cbs[i])->remote_id);
  CMD_DONE(SPATHA_CMD_EXECUTE_COMMANDS); }

static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool p, uint32_t first, uint32_t count)
{ CMD(cb); w_u64(&b, ID(p)); w_u64(&b, first); w_u64(&b, count); CMD_DONE(SPATHA_CMD_RESET_QUERY_POOL); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdBeginQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f)
{ CMD(cb); w_u64(&b, ID(p)); w_u64(&b, q); w_u64(&b, f); CMD_DONE(SPATHA_CMD_BEGIN_QUERY); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdEndQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q)
{ CMD(cb); w_u64(&b, ID(p)); w_u64(&b, q); CMD_DONE(SPATHA_CMD_END_QUERY); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool p, uint32_t q)
{ CMD(cb); w_u64(&b, st); w_u64(&b, ID(p)); w_u64(&b, q); CMD_DONE(SPATHA_CMD_WRITE_TIMESTAMP); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdCopyQueryPoolResults(VkCommandBuffer cb, VkQueryPool p, uint32_t first, uint32_t count, VkBuffer dst,
                               VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags f)
{ CMD(cb); w_u64(&b, ID(p)); w_u64(&b, first); w_u64(&b, count); w_u64(&b, ID(dst)); w_u64(&b, off);
  w_u64(&b, stride); w_u64(&b, f); CMD_DONE(SPATHA_CMD_COPY_QUERY_RESULTS); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkQueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo *i, VkFence f)
{ (void)q; (void)n; (void)i; (void)f; return VK_ERROR_FEATURE_NOT_PRESENT; }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateSamplerYcbcrConversion(VkDevice d, const VkSamplerYcbcrConversionCreateInfo *ci,
                                    const VkAllocationCallbacks *a, VkSamplerYcbcrConversion *out)
{ (void)d; (void)ci; (void)a; if (out) *out = VK_NULL_HANDLE; return VK_ERROR_FEATURE_NOT_PRESENT; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroySamplerYcbcrConversion(VkDevice d, VkSamplerYcbcrConversion c, const VkAllocationCallbacks *a)
{ (void)d; (void)c; (void)a; }

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *i, VkQueue *out)
{
    if (i->flags) { *out = VK_NULL_HANDLE; return; }
    stub_vkGetDeviceQueue(device, i->queueFamilyIndex, i->queueIndex, out);
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetRenderAreaGranularity(VkDevice d, VkRenderPass rp, VkExtent2D *g)
{ (void)d; (void)rp; if (g) { g->width = 1; g->height = 1; } }

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetDeviceGroupPeerMemoryFeatures(VkDevice d, uint32_t heap, uint32_t li, uint32_t ri, VkPeerMemoryFeatureFlags *f)
{ (void)d; (void)heap; (void)li; (void)ri; if (f) *f = 0; }

#define NEWFUNCS(X) \
  X(GetPhysicalDeviceQueueFamilyProperties2) X(GetPhysicalDeviceMemoryProperties2) \
  X(GetPhysicalDeviceFormatProperties2) X(GetPhysicalDeviceImageFormatProperties2) \
  X(GetPhysicalDeviceSparseImageFormatProperties2) X(GetPhysicalDeviceExternalBufferProperties) \
  X(GetPhysicalDeviceExternalFenceProperties) X(GetPhysicalDeviceExternalSemaphoreProperties) \
  X(EnumeratePhysicalDeviceGroups) \
  X(GetBufferMemoryRequirements2) X(GetImageMemoryRequirements2) X(GetImageSparseMemoryRequirements) \
  X(GetImageSparseMemoryRequirements2) X(BindBufferMemory2) X(BindImageMemory2) \
  X(GetDeviceMemoryCommitment) X(GetBufferDeviceAddress) X(GetBufferOpaqueCaptureAddress) \
  X(GetDeviceMemoryOpaqueCaptureAddress) \
  X(GetSemaphoreCounterValue) X(WaitSemaphores) X(SignalSemaphore) \
  X(CreateEvent) X(DestroyEvent) X(GetEventStatus) X(SetEvent) X(ResetEvent) \
  X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) X(ResetQueryPool) \
  X(CreateBufferView) X(DestroyBufferView) \
  X(GetPipelineCacheData) X(MergePipelineCaches) X(CreateComputePipelines) \
  X(ResetCommandPool) X(ResetDescriptorPool) X(TrimCommandPool) \
  X(GetDescriptorSetLayoutSupport) X(CreateDescriptorUpdateTemplate) \
  X(DestroyDescriptorUpdateTemplate) X(UpdateDescriptorSetWithTemplate) \
  X(CreateRenderPass2) X(CmdBeginRenderPass2) X(CmdNextSubpass2) X(CmdEndRenderPass2) \
  X(CmdDispatch) X(CmdDispatchIndirect) X(CmdDispatchBase) X(CmdSetDeviceMask) \
  X(CmdDrawIndirect) X(CmdDrawIndexedIndirect) X(CmdDrawIndirectCount) X(CmdDrawIndexedIndirectCount) \
  X(CmdClearDepthStencilImage) X(CmdClearAttachments) X(CmdResolveImage) X(CmdUpdateBuffer) \
  X(CmdSetDepthBounds) X(CmdSetStencilCompareMask) X(CmdSetStencilWriteMask) X(CmdSetStencilReference) \
  X(CmdSetEvent) X(CmdResetEvent) X(CmdWaitEvents) X(CmdExecuteCommands) \
  X(CmdResetQueryPool) X(CmdBeginQuery) X(CmdEndQuery) X(CmdWriteTimestamp) X(CmdCopyQueryPoolResults) \
  X(QueueBindSparse) X(CreateSamplerYcbcrConversion) X(DestroySamplerYcbcrConversion) \
  X(GetDeviceQueue2) X(GetRenderAreaGranularity) X(GetDeviceGroupPeerMemoryFeatures)

struct spatha_alias { const char *name; PFN_vkVoidFunction fn; };
#define AL(alias, core) { "vk" #alias, (PFN_vkVoidFunction)stub_vk##core },
static const struct spatha_alias g_alias[] = {
    AL(GetPhysicalDeviceFormatProperties2KHR, GetPhysicalDeviceFormatProperties2)
    AL(GetPhysicalDeviceImageFormatProperties2KHR, GetPhysicalDeviceImageFormatProperties2)
    AL(GetPhysicalDeviceQueueFamilyProperties2KHR, GetPhysicalDeviceQueueFamilyProperties2)
    AL(GetPhysicalDeviceMemoryProperties2KHR, GetPhysicalDeviceMemoryProperties2)
    AL(GetPhysicalDeviceSparseImageFormatProperties2KHR, GetPhysicalDeviceSparseImageFormatProperties2)
    AL(EnumeratePhysicalDeviceGroupsKHR, EnumeratePhysicalDeviceGroups)
    AL(GetPhysicalDeviceExternalBufferPropertiesKHR, GetPhysicalDeviceExternalBufferProperties)
    AL(GetPhysicalDeviceExternalFencePropertiesKHR, GetPhysicalDeviceExternalFenceProperties)
    AL(GetPhysicalDeviceExternalSemaphorePropertiesKHR, GetPhysicalDeviceExternalSemaphoreProperties)
    AL(BindBufferMemory2KHR, BindBufferMemory2)
    AL(BindImageMemory2KHR, BindImageMemory2)
    AL(GetBufferMemoryRequirements2KHR, GetBufferMemoryRequirements2)
    AL(GetImageMemoryRequirements2KHR, GetImageMemoryRequirements2)
    AL(GetImageSparseMemoryRequirements2KHR, GetImageSparseMemoryRequirements2)
    AL(TrimCommandPoolKHR, TrimCommandPool)
    AL(GetDescriptorSetLayoutSupportKHR, GetDescriptorSetLayoutSupport)
    AL(CreateDescriptorUpdateTemplateKHR, CreateDescriptorUpdateTemplate)
    AL(DestroyDescriptorUpdateTemplateKHR, DestroyDescriptorUpdateTemplate)
    AL(UpdateDescriptorSetWithTemplateKHR, UpdateDescriptorSetWithTemplate)
    AL(CreateRenderPass2KHR, CreateRenderPass2)
    AL(CmdBeginRenderPass2KHR, CmdBeginRenderPass2)
    AL(CmdNextSubpass2KHR, CmdNextSubpass2)
    AL(CmdEndRenderPass2KHR, CmdEndRenderPass2)
    AL(CmdDrawIndirectCountKHR, CmdDrawIndirectCount)
    AL(CmdDrawIndexedIndirectCountKHR, CmdDrawIndexedIndirectCount)
    AL(ResetQueryPoolEXT, ResetQueryPool)
    AL(GetSemaphoreCounterValueKHR, GetSemaphoreCounterValue)
    AL(WaitSemaphoresKHR, WaitSemaphores)
    AL(SignalSemaphoreKHR, SignalSemaphore)
    AL(GetBufferDeviceAddressKHR, GetBufferDeviceAddress)
    AL(GetBufferDeviceAddressEXT, GetBufferDeviceAddress)
    AL(GetBufferOpaqueCaptureAddressKHR, GetBufferOpaqueCaptureAddress)
    AL(GetDeviceMemoryOpaqueCaptureAddressKHR, GetDeviceMemoryOpaqueCaptureAddress)
    AL(CmdDispatchBaseKHR, CmdDispatchBase)
    AL(CmdSetDeviceMaskKHR, CmdSetDeviceMask)
    AL(GetDeviceGroupPeerMemoryFeaturesKHR, GetDeviceGroupPeerMemoryFeatures)
};
