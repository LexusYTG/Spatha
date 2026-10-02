case SPATHA_CMD_DISPATCH: {
    uint32_t x=(uint32_t)r_u64(&br), y=(uint32_t)r_u64(&br), z=(uint32_t)r_u64(&br);
    CMD_CHK(); D.CmdDispatch(cb,x,y,z); break; }
case SPATHA_CMD_DISPATCH_INDIRECT: {
    VkBuffer b=HV(SPATHA_T_BUF,VkBuffer,r_u64(&br)); VkDeviceSize o=r_u64(&br);
    CMD_CHK(); D.CmdDispatchIndirect(cb,b,o); break; }
case SPATHA_CMD_DISPATCH_BASE: {
    uint32_t a=(uint32_t)r_u64(&br),b=(uint32_t)r_u64(&br),c_=(uint32_t)r_u64(&br);
    uint32_t d=(uint32_t)r_u64(&br),e=(uint32_t)r_u64(&br),f=(uint32_t)r_u64(&br);
    CMD_CHK(); if (!D.CmdDispatchBase) { ok=0; break; }
    D.CmdDispatchBase(cb,a,b,c_,d,e,f); break; }
case SPATHA_CMD_DRAW_INDIRECT: {
    VkBuffer b=HV(SPATHA_T_BUF,VkBuffer,r_u64(&br)); VkDeviceSize o=r_u64(&br);
    uint32_t n=(uint32_t)r_u64(&br),s=(uint32_t)r_u64(&br);
    CMD_CHK(); D.CmdDrawIndirect(cb,b,o,n,s); break; }
case SPATHA_CMD_DRAW_INDEXED_INDIRECT: {
    VkBuffer b=HV(SPATHA_T_BUF,VkBuffer,r_u64(&br)); VkDeviceSize o=r_u64(&br);
    uint32_t n=(uint32_t)r_u64(&br),s=(uint32_t)r_u64(&br);
    CMD_CHK(); D.CmdDrawIndexedIndirect(cb,b,o,n,s); break; }
case SPATHA_CMD_DRAW_INDIRECT_COUNT: {
    VkBuffer b=HV(SPATHA_T_BUF,VkBuffer,r_u64(&br)); VkDeviceSize o=r_u64(&br);
    VkBuffer c2=HV(SPATHA_T_BUF,VkBuffer,r_u64(&br)); VkDeviceSize co=r_u64(&br);
    uint32_t mx=(uint32_t)r_u64(&br),s=(uint32_t)r_u64(&br);
    CMD_CHK(); if (!D.CmdDrawIndirectCount) { ok=0; break; }
    D.CmdDrawIndirectCount(cb,b,o,c2,co,mx,s); break; }
case SPATHA_CMD_DRAW_INDEXED_INDIRECT_COUNT: {
    VkBuffer b=HV(SPATHA_T_BUF,VkBuffer,r_u64(&br)); VkDeviceSize o=r_u64(&br);
    VkBuffer c2=HV(SPATHA_T_BUF,VkBuffer,r_u64(&br)); VkDeviceSize co=r_u64(&br);
    uint32_t mx=(uint32_t)r_u64(&br),s=(uint32_t)r_u64(&br);
    CMD_CHK(); if (!D.CmdDrawIndexedIndirectCount) { ok=0; break; }
    D.CmdDrawIndexedIndirectCount(cb,b,o,c2,co,mx,s); break; }
case SPATHA_CMD_CLEAR_DEPTH_STENCIL: {
    VkImage im=HV(SPATHA_T_IMG,VkImage,r_u64(&br)); VkImageLayout l=xl((VkImageLayout)r_u64(&br));
    RD_STRUCT(br,VkClearDepthStencilValue,v);
    uint64_t n=r_u64(&br); const void *rg=r_arr(&br,n,sizeof(VkImageSubresourceRange));
    CMD_CHK(); D.CmdClearDepthStencilImage(cb,im,l,&v,(uint32_t)n,rg); break; }
case SPATHA_CMD_CLEAR_ATTACHMENTS: {
    uint64_t na=r_u64(&br); const void *at=r_arr(&br,na,sizeof(VkClearAttachment));
    uint64_t nr=r_u64(&br); const void *rc=r_arr(&br,nr,sizeof(VkClearRect));
    CMD_CHK(); D.CmdClearAttachments(cb,(uint32_t)na,at,(uint32_t)nr,rc); break; }
case SPATHA_CMD_RESOLVE_IMAGE: {
    VkImage si=HV(SPATHA_T_IMG,VkImage,r_u64(&br)); VkImageLayout sl=xl((VkImageLayout)r_u64(&br));
    VkImage di=HV(SPATHA_T_IMG,VkImage,r_u64(&br)); VkImageLayout dl=xl((VkImageLayout)r_u64(&br));
    uint64_t n=r_u64(&br); const void *rg=r_arr(&br,n,sizeof(VkImageResolve));
    CMD_CHK(); D.CmdResolveImage(cb,si,sl,di,dl,(uint32_t)n,rg); break; }
case SPATHA_CMD_UPDATE_BUFFER: {
    VkBuffer b=HV(SPATHA_T_BUF,VkBuffer,r_u64(&br));
    VkDeviceSize o=r_u64(&br),sz=r_u64(&br);
    const void *d=r_blob(&br,sz);
    CMD_CHK(); D.CmdUpdateBuffer(cb,b,o,sz,d); break; }
case SPATHA_CMD_SET_DEPTH_BOUNDS: {
    const float *f=r_blob(&br,8); CMD_CHK(); D.CmdSetDepthBounds(cb,f[0],f[1]); break; }
case SPATHA_CMD_SET_STENCIL_COMPARE: {
    VkStencilFaceFlags f=(VkStencilFaceFlags)r_u64(&br); uint32_t v=(uint32_t)r_u64(&br);
    CMD_CHK(); D.CmdSetStencilCompareMask(cb,f,v); break; }
case SPATHA_CMD_SET_STENCIL_WRITE: {
    VkStencilFaceFlags f=(VkStencilFaceFlags)r_u64(&br); uint32_t v=(uint32_t)r_u64(&br);
    CMD_CHK(); D.CmdSetStencilWriteMask(cb,f,v); break; }
case SPATHA_CMD_SET_STENCIL_REF: {
    VkStencilFaceFlags f=(VkStencilFaceFlags)r_u64(&br); uint32_t v=(uint32_t)r_u64(&br);
    CMD_CHK(); D.CmdSetStencilReference(cb,f,v); break; }
case SPATHA_CMD_SET_EVENT: {
    VkEvent e=HV(SPATHA_T_EVENT,VkEvent,r_u64(&br)); VkPipelineStageFlags m=(VkPipelineStageFlags)r_u64(&br);
    CMD_CHK(); D.CmdSetEvent(cb,e,m); break; }
case SPATHA_CMD_RESET_EVENT: {
    VkEvent e=HV(SPATHA_T_EVENT,VkEvent,r_u64(&br)); VkPipelineStageFlags m=(VkPipelineStageFlags)r_u64(&br);
    CMD_CHK(); D.CmdResetEvent(cb,e,m); break; }
case SPATHA_CMD_WAIT_EVENTS: {
    uint64_t ne=r_u64(&br); const uint64_t *eids=r_arr(&br,ne,8);
    VkPipelineStageFlags ss=(VkPipelineStageFlags)r_u64(&br),ds=(VkPipelineStageFlags)r_u64(&br);
    uint64_t nm=r_u64(&br); const void *mb=r_arr(&br,nm,sizeof(VkMemoryBarrier));
    uint64_t nb=r_u64(&br); const void *bb=r_arr(&br,nb,sizeof(VkBufferMemoryBarrier));
    uint64_t ni=r_u64(&br); const void *ib=r_arr(&br,ni,sizeof(VkImageMemoryBarrier));
    CMD_CHK();
    VkEvent *ev=ar_alloc(c,ne*sizeof(*ev)+8);
    for (uint64_t i=0;i<ne;i++) ev[i]=HV(SPATHA_T_EVENT,VkEvent,eids[i]);
    VkMemoryBarrier *m2=ar_dup(c,mb,nm*sizeof(*m2));
    for (uint64_t i=0;i<nm;i++) m2[i].pNext=NULL;
    VkBufferMemoryBarrier *b2=ar_dup(c,bb,nb*sizeof(*b2));
    for (uint64_t i=0;i<nb;i++){ b2[i].pNext=NULL; b2[i].buffer=HV(SPATHA_T_BUF,VkBuffer,H2U(b2[i].buffer)); }
    VkImageMemoryBarrier *i2=ar_dup(c,ib,ni*sizeof(*i2));
    for (uint64_t i=0;i<ni;i++){ i2[i].pNext=NULL; i2[i].image=HV(SPATHA_T_IMG,VkImage,H2U(i2[i].image));
        i2[i].oldLayout=xl(i2[i].oldLayout); i2[i].newLayout=xl(i2[i].newLayout); }
    CMD_CHK(); D.CmdWaitEvents(cb,(uint32_t)ne,ev,ss,ds,(uint32_t)nm,m2,(uint32_t)nb,b2,(uint32_t)ni,i2); break; }
case SPATHA_CMD_EXECUTE_COMMANDS: {
    uint64_t n=r_u64(&br); const uint64_t *ids=r_arr(&br,n,8);
    CMD_CHK();
    VkCommandBuffer *cbs=ar_alloc(c,n*sizeof(*cbs)+8);
    for (uint64_t i=0;i<n;i++) cbs[i]=HV(SPATHA_T_CMDBUF,VkCommandBuffer,ids[i]);
    CMD_CHK(); D.CmdExecuteCommands(cb,(uint32_t)n,cbs); break; }
case SPATHA_CMD_RESET_QUERY_POOL: {
    VkQueryPool q=HV(SPATHA_T_QPOOL,VkQueryPool,r_u64(&br));
    uint32_t f=(uint32_t)r_u64(&br),n=(uint32_t)r_u64(&br);
    CMD_CHK(); D.CmdResetQueryPool(cb,q,f,n); break; }
case SPATHA_CMD_BEGIN_QUERY: {
    VkQueryPool q=HV(SPATHA_T_QPOOL,VkQueryPool,r_u64(&br));
    uint32_t i=(uint32_t)r_u64(&br); VkQueryControlFlags f=(VkQueryControlFlags)r_u64(&br);
    CMD_CHK(); D.CmdBeginQuery(cb,q,i,f); break; }
case SPATHA_CMD_END_QUERY: {
    VkQueryPool q=HV(SPATHA_T_QPOOL,VkQueryPool,r_u64(&br)); uint32_t i=(uint32_t)r_u64(&br);
    CMD_CHK(); D.CmdEndQuery(cb,q,i); break; }
case SPATHA_CMD_WRITE_TIMESTAMP: {
    VkPipelineStageFlagBits st=(VkPipelineStageFlagBits)r_u64(&br);
    VkQueryPool q=HV(SPATHA_T_QPOOL,VkQueryPool,r_u64(&br)); uint32_t i=(uint32_t)r_u64(&br);
    CMD_CHK(); D.CmdWriteTimestamp(cb,st,q,i); break; }
case SPATHA_CMD_COPY_QUERY_RESULTS: {
    VkQueryPool q=HV(SPATHA_T_QPOOL,VkQueryPool,r_u64(&br));
    uint32_t f=(uint32_t)r_u64(&br),n=(uint32_t)r_u64(&br);
    VkBuffer d=HV(SPATHA_T_BUF,VkBuffer,r_u64(&br));
    VkDeviceSize o=r_u64(&br),s=r_u64(&br); VkQueryResultFlags fl=(VkQueryResultFlags)r_u64(&br);
    CMD_CHK(); D.CmdCopyQueryPoolResults(cb,q,f,n,d,o,s,fl); break; }
