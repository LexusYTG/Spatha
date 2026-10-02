/*
 * spathad_v12.inc — handlers Vulkan 1.1/1.2 del daemon.
 * Se incluye en spathad.c antes de "creacion de objetos simples".
 */

static int has_extras(uint32_t t) {
    return t == SPATHA_T_SEM || t == SPATHA_T_DSL || t == SPATHA_T_SAMPLER || t == SPATHA_T_VIEW ||
           t == SPATHA_T_IMG || t == SPATHA_T_RPASS || t == SPATHA_T_EVENT || t == SPATHA_T_QPOOL ||
           t == SPATHA_T_BVIEW;
}

static const void *xr(struct client *c, struct rbuf *r, uint32_t type) {
    if (!has_extras(type)) return NULL;
    uint64_t mask = r_u64(r);
    if (r->err || !mask) return NULL;
    switch (type) {
    case SPATHA_T_SEM:
        if (mask & 1) {
            VkSemaphoreTypeCreateInfo *t = ar_alloc(c, sizeof(*t));
            t->sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            t->semaphoreType = (VkSemaphoreType)r_u64(r);
            t->initialValue = r_u64(r);
            return t;
        }
        break;
    case SPATHA_T_DSL:
        if (mask & 1) {
            uint64_t n = r_u64(r);
            if (n > 1024) { r->err = 1; return NULL; }
            VkDescriptorBindingFlags *f = ar_alloc(c, n * sizeof(*f) + 8);
            for (uint64_t i = 0; i < n; i++) f[i] = (VkDescriptorBindingFlags)r_u64(r);
            VkDescriptorSetLayoutBindingFlagsCreateInfo *t = ar_alloc(c, sizeof(*t));
            t->sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
            t->bindingCount = (uint32_t)n; t->pBindingFlags = f;
            return t;
        }
        break;
    case SPATHA_T_SAMPLER:
        if (mask & 1) {
            VkSamplerReductionModeCreateInfo *t = ar_alloc(c, sizeof(*t));
            t->sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO;
            t->reductionMode = (VkSamplerReductionMode)r_u64(r);
            return t;
        }
        break;
    case SPATHA_T_VIEW:
        if (mask & 1) {
            VkImageViewUsageCreateInfo *t = ar_alloc(c, sizeof(*t));
            t->sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
            t->usage = (VkImageUsageFlags)r_u64(r);
            return t;
        }
        break;
    case SPATHA_T_IMG: {
        VkImageStencilUsageCreateInfo *su = NULL;
        VkImageFormatListCreateInfo *fl = NULL;
        if (mask & 1) {
            su = ar_alloc(c, sizeof(*su));
            su->sType = VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO;
            su->stencilUsage = (VkImageUsageFlags)r_u64(r);
        }
        if (mask & 2) {
            uint64_t n = r_u64(r);
            if (n > 64) { r->err = 1; return NULL; }
            VkFormat *f = ar_alloc(c, n * sizeof(*f) + 8);
            for (uint64_t i = 0; i < n; i++) f[i] = (VkFormat)r_u64(r);
            fl = ar_alloc(c, sizeof(*fl));
            fl->sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
            fl->viewFormatCount = (uint32_t)n; fl->pViewFormats = f;
        }
        if (su) { su->pNext = fl; return su; }
        return fl; }
    case SPATHA_T_RPASS: {
        VkRenderPassMultiviewCreateInfo *mv = NULL;
        VkRenderPassInputAttachmentAspectCreateInfo *ia = NULL;
        if (mask & 1) {
            uint64_t nm = r_u64(r);
            if (nm > 256) { r->err = 1; return NULL; }
            uint32_t *vm = ar_alloc(c, nm * 4 + 8);
            for (uint64_t i = 0; i < nm; i++) vm[i] = (uint32_t)r_u64(r);
            uint64_t no = r_u64(r);
            if (no > 1024) { r->err = 1; return NULL; }
            int32_t *vo = ar_alloc(c, no * 4 + 8);
            for (uint64_t i = 0; i < no; i++) vo[i] = (int32_t)(int64_t)r_u64(r);
            uint64_t nc = r_u64(r);
            if (nc > 256) { r->err = 1; return NULL; }
            uint32_t *cm = ar_alloc(c, nc * 4 + 8);
            for (uint64_t i = 0; i < nc; i++) cm[i] = (uint32_t)r_u64(r);
            mv = ar_alloc(c, sizeof(*mv));
            mv->sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
            mv->subpassCount = (uint32_t)nm;     mv->pViewMasks = nm ? vm : NULL;
            mv->dependencyCount = (uint32_t)no;  mv->pViewOffsets = no ? vo : NULL;
            mv->correlationMaskCount = (uint32_t)nc; mv->pCorrelationMasks = nc ? cm : NULL;
        }
        if (mask & 2) {
            uint64_t n = r_u64(r);
            const void *a = r_arr(r, n, sizeof(VkInputAttachmentAspectReference));
            if (r->err) return NULL;
            ia = ar_alloc(c, sizeof(*ia));
            ia->sType = VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO;
            ia->aspectReferenceCount = (uint32_t)n; ia->pAspectReferences = a;
        }
        if (mv) { mv->pNext = ia; return mv; }
        return ia; }
    default: break;
    }
    return NULL;
}

static int dsl_read_body(struct client *c, struct rbuf *r, VkDescriptorSetLayoutCreateInfo *out, const void *xnext) {
    RD_STRUCT(*r, VkDescriptorSetLayoutCreateInfo, ci);
    const VkDescriptorSetLayoutBinding *in = r_arr(r, ci.bindingCount, sizeof(VkDescriptorSetLayoutBinding));
    if (r->err) return 0;
    VkDescriptorSetLayoutBinding *b = ar_dup(c, in, ci.bindingCount * sizeof(*b));
    for (uint32_t i = 0; i < ci.bindingCount; i++) {
        if (!b[i].pImmutableSamplers) continue;
        const uint64_t *ids = r_arr(r, b[i].descriptorCount, 8);
        if (r->err) return 0;
        VkSampler *sm = ar_alloc(c, b[i].descriptorCount * sizeof(VkSampler) + 8);
        for (uint32_t k = 0; k < b[i].descriptorCount; k++) sm[k] = HV(SPATHA_T_SAMPLER, VkSampler, ids[k]);
        b[i].pImmutableSamplers = sm;
    }
    if (V.bad) return 0;
    ci.pNext = xnext; ci.pBindings = b;
    *out = ci;
    return 1;
}

static int h_pd_chain(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t id = r_u64(&r), kind = r_u64(&r), cnt = r_u64(&r);
    int slot = find_pd(c, (uint32_t)id);
    const uint64_t *ty = (cnt && cnt <= 64) ? r_arr(&r, cnt, 8) : NULL;
    if (r.err || slot < 0 || !ty) return reply_res(c, m, VK_ERROR_UNKNOWN);
    if (kind == 0 ? !p_vkGetPhysicalDeviceFeatures2 : !p_vkGetPhysicalDeviceProperties2)
        return reply_res(c, m, VK_ERROR_FEATURE_NOT_PRESENT);

    uint8_t **bl = ar_alloc(c, cnt * sizeof(*bl));
    size_t *sz = ar_alloc(c, cnt * sizeof(*sz));
    VkBaseOutStructure *head = NULL, *tail = NULL;
    for (uint64_t k = 0; k < cnt; k++) {
        sz[k] = spatha_chain_size((VkStructureType)ty[k]);
        if (!sz[k]) { ar_free(c); return reply_res(c, m, VK_ERROR_UNKNOWN); }
        bl[k] = ar_alloc(c, sz[k]);
        VkBaseOutStructure *s = (VkBaseOutStructure *)bl[k];
        s->sType = (VkStructureType)ty[k];
        if (tail) tail->pNext = s; else head = s;
        tail = s;
    }
    VkPhysicalDevice pd = c->devices[slot];
    if (kind == 0) {
        VkPhysicalDeviceFeatures2 f2; memset(&f2, 0, sizeof(f2));
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; f2.pNext = head;
        p_vkGetPhysicalDeviceFeatures2(pd, &f2);
    } else {
        VkPhysicalDeviceProperties2 p2; memset(&p2, 0, sizeof(p2));
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; p2.pNext = head;
        p_vkGetPhysicalDeviceProperties2(pd, &p2);
    }
    struct wbuf w; w_init(&w);
    for (uint64_t k = 0; k < cnt; k++) w_raw(&w, bl[k], sz[k]);
    ar_free(c);
    int rc = reply_data(c, m, VK_SUCCESS, w.p, w.len);
    w_free(&w);
    return rc;
}

static int h_sem_op(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), op = r_u64(&r), val = 0;
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    if (r.err || !V.dev || dev != V.dev_id) goto out;
    if (!D.WaitSemaphores) { vr = VK_ERROR_FEATURE_NOT_PRESENT; goto out; }
    if (op == 0) {
        VkSemaphore s = HV(SPATHA_T_SEM, VkSemaphore, r_u64(&r));
        if (!r.err && !V.bad) vr = D.GetSemaphoreCounterValue(V.dev, s, &val);
    } else if (op == 1) {
        uint64_t flags = r_u64(&r), to = r_u64(&r), n = r_u64(&r);
        const uint64_t *ids = (n && n <= 64) ? r_arr(&r, n, 8) : NULL;
        const uint64_t *vals = ids ? r_arr(&r, n, 8) : NULL;
        if (!r.err && ids && vals) {
            VkSemaphore *ss = ar_alloc(c, n * sizeof(*ss) + 8);
            for (uint64_t i = 0; i < n; i++) ss[i] = HV(SPATHA_T_SEM, VkSemaphore, ids[i]);
            if (!V.bad) {
                VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                    .flags = (VkSemaphoreWaitFlags)flags, .semaphoreCount = (uint32_t)n,
                    .pSemaphores = ss, .pValues = vals };
                vr = D.WaitSemaphores(V.dev, &wi, to);
            }
        }
    } else if (op == 2) {
        VkSemaphore s = HV(SPATHA_T_SEM, VkSemaphore, r_u64(&r));
        uint64_t v = r_u64(&r);
        if (!r.err && !V.bad) {
            VkSemaphoreSignalInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                .semaphore = s, .value = v };
            vr = D.SignalSemaphore(V.dev, &si);
        }
    }
out:;
    ar_free(c);
    return reply_data(c, m, vr, &val, 8);
}

static int h_event_op(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), op = r_u64(&r), eid = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    if (!r.err && V.dev && dev == V.dev_id) {
        VkEvent e = HV(SPATHA_T_EVENT, VkEvent, eid);
        if (!V.bad) {
            if (op == 0) vr = D.GetEventStatus(V.dev, e);
            else if (op == 1) vr = D.SetEvent(V.dev, e);
            else if (op == 2) vr = D.ResetEvent(V.dev, e);
        }
    }
    return reply_res(c, m, vr);
}

static int h_query_results(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), pool = r_u64(&r), first = r_u64(&r), count = r_u64(&r);
    uint64_t dsz = r_u64(&r), stride = r_u64(&r), flags = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    uint8_t *buf = NULL;
    V.bad = 0;
    if (!r.err && V.dev && dev == V.dev_id && dsz <= SPATHA_MAX_PAYLOAD - 128) {
        VkQueryPool qp = HV(SPATHA_T_QPOOL, VkQueryPool, pool);
        if (!V.bad) {
            buf = calloc(1, dsz ? dsz : 1);
            if (!buf) oom();
            vr = D.GetQueryPoolResults(V.dev, qp, (uint32_t)first, (uint32_t)count, (size_t)dsz, buf,
                                       stride, (VkQueryResultFlags)flags);
        }
    }
    int ok = vr == VK_SUCCESS || vr == VK_NOT_READY;
    int rc = reply_data(c, m, vr, buf, ok ? dsz : 0);
    free(buf);
    return rc;
}

static int h_reset_query(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), pool = r_u64(&r), first = r_u64(&r), count = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    if (!r.err && V.dev && dev == V.dev_id) {
        VkQueryPool qp = HV(SPATHA_T_QPOOL, VkQueryPool, pool);
        if (!V.bad && D.ResetQueryPool) { D.ResetQueryPool(V.dev, qp, (uint32_t)first, (uint32_t)count); vr = VK_SUCCESS; }
        else if (!V.bad) vr = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return reply_res(c, m, vr);
}

static int h_pcache_op(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), op = r_u64(&r), id = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    uint8_t *out = NULL; size_t outlen = 0;
    V.bad = 0;
    if (r.err || !V.dev || dev != V.dev_id) goto done;
    if (op == 0) {
        uint64_t cap = r_u64(&r), want = r_u64(&r);
        VkPipelineCache pc = HV(SPATHA_T_PCACHE, VkPipelineCache, id);
        if (r.err || V.bad) goto done;
        if (cap > SPATHA_MAX_PAYLOAD - 128) cap = SPATHA_MAX_PAYLOAD - 128;
        size_t sz = want ? (size_t)cap : 0;
        uint8_t *buf = want ? malloc(cap ? cap : 1) : NULL;
        if (want && !buf) oom();
        vr = D.GetPipelineCacheData(V.dev, pc, &sz, buf);
        if (vr == VK_SUCCESS || vr == VK_INCOMPLETE) {
            size_t k = want ? (sz < cap ? sz : (size_t)cap) : 0;
            out = malloc(8 + k + 8);
            if (!out) oom();
            uint64_t s64 = sz;
            memcpy(out, &s64, 8);
            if (k) memcpy(out + 8, buf, k);
            outlen = 8 + k;
        }
        free(buf);
    } else if (op == 1) {
        uint64_t n = r_u64(&r);
        const uint64_t *ids = (n && n <= 64) ? r_arr(&r, n, 8) : NULL;
        VkPipelineCache dst = HV(SPATHA_T_PCACHE, VkPipelineCache, id);
        if (r.err || !ids) goto done;
        VkPipelineCache *src = ar_alloc(c, n * sizeof(*src) + 8);
        for (uint64_t i = 0; i < n; i++) src[i] = HV(SPATHA_T_PCACHE, VkPipelineCache, ids[i]);
        if (!V.bad) vr = D.MergePipelineCaches(V.dev, dst, (uint32_t)n, src);
    }
done:;
    ar_free(c);
    int rc = reply_data(c, m, vr, out, outlen);
    free(out);
    return rc;
}

static int rd_stage(struct client *c, struct rbuf *r, VkPipelineShaderStageCreateInfo *st) {
    RD_STRUCT(*r, VkPipelineShaderStageCreateInfo, s);
    uint64_t nl = r_u64(r);
    const char *nm = r_blob(r, nl);
    if (r->err) return 0;
    char *name = ar_alloc(c, nl + 1);
    if (nl) memcpy(name, nm, nl);
    s.pNext = NULL; s.pName = name;
    s.module = HV(SPATHA_T_SHADER, VkShaderModule, H2U(s.module));
    if (r_u64(r)) {
        RD_STRUCT(*r, VkSpecializationInfo, si);
        const void *me = r_arr(r, si.mapEntryCount, sizeof(VkSpecializationMapEntry));
        const void *dd = r_blob(r, si.dataSize);
        if (r->err) return 0;
        VkSpecializationInfo *sp = ar_alloc(c, sizeof(*sp));
        *sp = si; sp->pMapEntries = me; sp->pData = dd;
        s.pSpecializationInfo = sp;
    } else s.pSpecializationInfo = NULL;
    *st = s;
    return !V.bad;
}

static int h_create_compute(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), cache = r_u64(&r), count = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    VkPipeline *pipes = NULL;
    V.bad = 0;
    if (r.err || !V.dev || dev != V.dev_id || count == 0 || count > 64) goto out;
    VkComputePipelineCreateInfo *cis = ar_alloc(c, count * sizeof(*cis));
    pipes = ar_alloc(c, count * sizeof(*pipes));
    for (uint64_t k = 0; k < count; k++) {
        RD_STRUCT(r, VkComputePipelineCreateInfo, ci);
        if (r.err || !rd_stage(c, &r, &ci.stage)) goto out;
        ci.pNext = NULL;
        ci.layout = HV(SPATHA_T_PLAYOUT, VkPipelineLayout, H2U(ci.layout));
        ci.basePipelineHandle = HV(SPATHA_T_PIPE, VkPipeline, H2U(ci.basePipelineHandle));
        if (r.err || V.bad) goto out;
        cis[k] = ci;
    }
    VkPipelineCache pc = HV(SPATHA_T_PCACHE, VkPipelineCache, cache);
    if (V.bad) goto out;
    vr = D.CreateComputePipelines(V.dev, pc, (uint32_t)count, cis, NULL, pipes);
out:;
    struct wbuf w; w_init(&w);
    if (vr == VK_SUCCESS)
        for (uint64_t k = 0; k < count; k++) w_u64(&w, obj_add(c, SPATHA_T_PIPE, H2U(pipes[k]), 0, 0));
    ar_free(c);
    int rc = reply_data(c, m, vr, w.p, w.len);
    w_free(&w);
    return rc;
}

static int h_reset_pool(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), kind = r_u64(&r), id = r_u64(&r), flags = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    if (!r.err && V.dev && dev == V.dev_id) {
        struct obj *o = obj_find(c, (uint32_t)kind, id);
        if (o && kind == SPATHA_T_CMDPOOL)
            vr = D.ResetCommandPool(V.dev, U2H(VkCommandPool, o->h), (VkCommandPoolResetFlags)flags);
        else if (o && kind == SPATHA_T_DPOOL) {
            vr = D.ResetDescriptorPool(V.dev, U2H(VkDescriptorPool, o->h), (VkDescriptorPoolResetFlags)flags);
            if (vr == VK_SUCCESS) obj_purge_children(c, SPATHA_T_DSET, o->h);
        }
    }
    return reply_res(c, m, vr);
}

static int h_dsl_support(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r);
    V.bad = 0;
    const void *xn = xr(c, &r, SPATHA_T_DSL);
    VkDescriptorSetLayoutCreateInfo ci;
    if (r.err || !V.dev || dev != V.dev_id || !D.GetDescriptorSetLayoutSupport ||
        !dsl_read_body(c, &r, &ci, xn)) {
        ar_free(c);
        return reply_res(c, m, VK_ERROR_UNKNOWN);
    }
    VkDescriptorSetVariableDescriptorCountLayoutSupport vs; memset(&vs, 0, sizeof(vs));
    vs.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT;
    VkDescriptorSetLayoutSupport sup; memset(&sup, 0, sizeof(sup));
    sup.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT; sup.pNext = &vs;
    D.GetDescriptorSetLayoutSupport(V.dev, &ci, &sup);
    uint64_t out[2] = { sup.supported, vs.maxVariableDescriptorCount };
    ar_free(c);
    return reply_data(c, m, VK_SUCCESS, out, sizeof(out));
}

static VkAttachmentReference2 *ref2_dup(struct client *c, const void *src, uint32_t n) {
    VkAttachmentReference2 *x = ar_dup(c, src, n * sizeof(*x));
    for (uint32_t k = 0; k < n; k++) { x[k].pNext = NULL; x[k].layout = xl(x[k].layout); }
    return x;
}

static int h_create_rp2(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), id = 0;
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    if (r.err || !V.dev || dev != V.dev_id || !D.CreateRenderPass2) goto out;
    RD_STRUCT(r, VkRenderPassCreateInfo2, ci);
    const void *ain = r_arr(&r, ci.attachmentCount, sizeof(VkAttachmentDescription2));
    if (r.err) goto out;
    VkAttachmentDescription2 *att = ar_dup(c, ain, ci.attachmentCount * sizeof(*att));
    for (uint32_t i = 0; i < ci.attachmentCount; i++) {
        att[i].pNext = NULL;
        att[i].initialLayout = xl(att[i].initialLayout);
        att[i].finalLayout = xl(att[i].finalLayout);
    }
    VkSubpassDescription2 *sps = ar_alloc(c, ci.subpassCount * sizeof(*sps) + 8);
    for (uint32_t i = 0; i < ci.subpassCount; i++) {
        RD_STRUCT(r, VkSubpassDescription2, s);
        int has_res = s.pResolveAttachments != NULL, has_ds = s.pDepthStencilAttachment != NULL;
        const void *ia = r_arr(&r, s.inputAttachmentCount, sizeof(VkAttachmentReference2));
        const void *ca = r_arr(&r, s.colorAttachmentCount, sizeof(VkAttachmentReference2));
        const void *ra = has_res ? r_arr(&r, s.colorAttachmentCount, sizeof(VkAttachmentReference2)) : NULL;
        const void *da = has_ds ? r_arr(&r, 1, sizeof(VkAttachmentReference2)) : NULL;
        const uint32_t *pa = r_arr(&r, s.preserveAttachmentCount, 4);
        VkSubpassDescriptionDepthStencilResolve dr0; const void *dra = NULL;
        int has_dr = r_u64(&r) != 0;
        memset(&dr0, 0, sizeof(dr0));
        if (has_dr) {
            RD_STRUCT(r, VkSubpassDescriptionDepthStencilResolve, d1);
            dr0 = d1;
            dra = r_arr(&r, 1, sizeof(VkAttachmentReference2));
        }
        if (r.err) goto out;
        s.pNext = NULL;
        s.pInputAttachments = ref2_dup(c, ia, s.inputAttachmentCount);
        s.pColorAttachments = ref2_dup(c, ca, s.colorAttachmentCount);
        s.pResolveAttachments = has_res ? ref2_dup(c, ra, s.colorAttachmentCount) : NULL;
        s.pDepthStencilAttachment = has_ds ? ref2_dup(c, da, 1) : NULL;
        s.pPreserveAttachments = ar_dup(c, pa, s.preserveAttachmentCount * 4);
        if (has_dr) {
            VkSubpassDescriptionDepthStencilResolve *d = ar_alloc(c, sizeof(*d));
            *d = dr0; d->pNext = NULL;
            d->pDepthStencilResolveAttachment = ref2_dup(c, dra, 1);
            s.pNext = d;
        }
        sps[i] = s;
    }
    const void *din = r_arr(&r, ci.dependencyCount, sizeof(VkSubpassDependency2));
    const void *cmk = r_arr(&r, ci.correlatedViewMaskCount, 4);
    if (r.err) goto out;
    VkSubpassDependency2 *dep = ar_dup(c, din, ci.dependencyCount * sizeof(*dep));
    for (uint32_t i = 0; i < ci.dependencyCount; i++) dep[i].pNext = NULL;
    ci.pNext = NULL; ci.pAttachments = att; ci.pSubpasses = sps; ci.pDependencies = dep;
    ci.pCorrelatedViewMasks = cmk;
    VkRenderPass x = VK_NULL_HANDLE;
    vr = D.CreateRenderPass2(V.dev, &ci, NULL, &x);
    if (vr == VK_SUCCESS) id = obj_add(c, SPATHA_T_RPASS, H2U(x), 0, 0);
out:;
    ar_free(c);
    return reply_data(c, m, vr, &id, 8);
}

static int h_buffer_addr(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), bid = r_u64(&r), addr = 0;
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    if (!r.err && V.dev && dev == V.dev_id && D.GetBufferDeviceAddress) {
        VkBuffer b = HV(SPATHA_T_BUF, VkBuffer, bid);
        if (!V.bad) {
            VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b };
            addr = D.GetBufferDeviceAddress(V.dev, &ai);
            vr = VK_SUCCESS;
        }
    }
    return reply_data(c, m, vr, &addr, 8);
}
