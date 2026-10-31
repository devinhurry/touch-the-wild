// BOTW input identification follows MacNativeGames' scene/camera notes (MIT).
// UI uses Apple's composited-UI mode: provide the same frame before and after
// UI, without changing or replaying game draws. See WWDC25 session 211.
static struct {
    uint64_t frames;
    NSUInteger width,height,depth_width,depth_height;
    MTLPixelFormat format;
    id<MTLTexture> composited_canvas;
    int depth,want_camera,invalid;
    uint64_t candidate_count,profile_candidate;
    NSUInteger profile_w,profile_h;
    int profile_selected;
} g_framegen_capture_state;
static void framegen_resize_wait(const char *kind,NSUInteger old_w,NSUInteger old_h,NSUInteger w,NSUInteger h) {
    int profile=dm_fg_profile_on(); double a=profile?CACurrentMediaTime():0;
    mtl_present_flush(); double b=profile?CACurrentMediaTime():0;
    dm_fg_wait_idle(); double c=profile?CACurrentMediaTime():0;
    if (profile) dm_fg_profile_event(@"resize_wait",@{@"frame":@(atomic_load(&g_cur_frame)),@"kind":@(kind),@"old_w":@(old_w),@"old_h":@(old_h),@"w":@(w),@"h":@(h),@"display_drain_ms":@((b-a)*1000),@"generation_drain_ms":@((c-b)*1000)});
}
static void framegen_discard_inputs(void) {
    dm_fg_reset();
    g_framegen_capture_state.composited_canvas=nil;
    g_framegen_capture_state.depth=g_framegen_capture_state.want_camera=g_framegen_capture_state.invalid=0;
    g_framegen_capture_state.profile_candidate=0;
}
static void framegen_note_pass(MTL4RenderPassDescriptor *pd) {
    if (!dm_fg_available(g_dev)) return;
    int profile=dm_fg_profile_on();
    if (g_framegen_capture_state.depth && !profile) return;
    id<MTLTexture> color=pd.colorAttachments[0].texture, depth=pd.depthAttachment.texture;
    int colors=0;
    for (int i=0;i<8;i++) colors += pd.colorAttachments[i].texture!=nil;
    if (colors>=3 && color && depth && depth.pixelFormat==MTLPixelFormatDepth32Float &&
        depth.textureType==MTLTextureType2D && depth.sampleCount==1 &&
        depth.width==color.width && depth.height==color.height && depth.width>=320 && framegen_present_active()) {
        int selected=!g_framegen_capture_state.depth;
        if (profile) {
            g_framegen_capture_state.profile_candidate=++g_framegen_capture_state.candidate_count;
            g_framegen_capture_state.profile_w=depth.width; g_framegen_capture_state.profile_h=depth.height;
            g_framegen_capture_state.profile_selected=selected;
            dm_fg_profile_event(@"depth_candidate",@{@"frame":@(atomic_load(&g_cur_frame)),@"candidate":@(g_framegen_capture_state.profile_candidate),@"pass":@(g_stat_pass),@"w":@(depth.width),@"h":@(depth.height),@"colors":@(colors),@"selected":@(selected),@"depth_serial":@(R.rt.depth?R.rt.depth->serial:0),@"color_serial":@(R.rt.color[0]?R.rt.color[0]->serial:0)});
        }
        if (!selected) return;
        // Changing source depth dimensions are normalized by the motion kernel.
        // Only canvas/format changes replace display resources and need a drain.
        g_framegen_capture_state.depth_width=depth.width; g_framegen_capture_state.depth_height=depth.height;
        dm_fg_note_depth(depth); g_framegen_capture_state.depth=g_framegen_capture_state.want_camera=1;
    }
}
static void framegen_take_camera(NvProg *program) {
    if (dm_fg_profile_on() && g_framegen_capture_state.profile_candidate) {
        unsigned slot=0,bytes=0; sscanf(DM_GAME_FRAMEGEN_CAMERA,"%u:%u",&slot,&bytes);
        const p_bind_buf *u=slot<16?&S.ubo[0][slot]:NULL;
        dm_fg_profile_event(@"candidate_draw",@{@"frame":@(atomic_load(&g_cur_frame)),@"candidate":@(g_framegen_capture_state.profile_candidate),@"pass":@(g_stat_pass),@"w":@(g_framegen_capture_state.profile_w),@"h":@(g_framegen_capture_state.profile_h),@"selected":@(g_framegen_capture_state.profile_selected),@"vs":@(program&&program->sh[0]?program->sh[0]->key:""),@"fs":@(program&&program->sh[1]?program->sh[1]->key:""),@"ubo_address":@(u?u->addr:0),@"ubo_bytes":@(u?u->size:0)});
        g_framegen_capture_state.profile_candidate=0;
    }
    if (!g_framegen_capture_state.want_camera) return;
    unsigned slot=0,bytes=0;
    if (sscanf(DM_GAME_FRAMEGEN_CAMERA,"%u:%u",&slot,&bytes)!=2 || slot>=16) return;
    const p_bind_buf *uniform=&S.ubo[0][slot];
    if (!uniform->addr || uniform->size!=bytes) return;
    float camera[84]; ubo_read(0,slot,0,sizeof camera,camera);
    if (dm_fg_profile_on()) {
        int valid=1; for (int i=0;i<84;i++) valid &= isfinite(camera[i]);
        NSMutableDictionary *row=[@{@"frame":@(atomic_load(&g_cur_frame)),@"pass":@(g_stat_pass),@"w":@(g_framegen_capture_state.depth_width),@"h":@(g_framegen_capture_state.depth_height),@"ubo_address":@(uniform->addr),@"ubo_bytes":@(uniform->size),@"finite":@(valid),@"vs":@(program&&program->sh[0]?program->sh[0]->key:""),@"fs":@(program&&program->sh[1]?program->sh[1]->key:"")} mutableCopy];
        if (valid) { row[@"camera_x"]=@(camera[47]); row[@"camera_y"]=@(camera[51]); row[@"camera_z"]=@(camera[55]); row[@"near"]=@(camera[56]); row[@"far"]=@(camera[57]); row[@"aspect"]=@(camera[62]); row[@"fovy"]=@(camera[70]); }
        dm_fg_profile_event(@"camera_capture",row);
    }
    dm_fg_note_camera(camera,84); g_framegen_capture_state.want_camera=0;
}
static void framegen_after_draw(NvProg *program) {
    if (!dm_fg_available(g_dev) || !program || !program->sh[1] || R.rt.n<1 || !R.rt.color[0] ||
        strncmp(program->sh[1]->key,DM_GAME_FRAMEGEN_SCENE,16) || !framegen_present_active()) return;
    rp_end();
    id<MTLTexture> canvas=tex_mtl(R.rt.color[0]);
    if (!canvas || canvas.textureType!=MTLTextureType2D || canvas.sampleCount!=1) { g_framegen_capture_state.invalid=1; return; }
    if (g_framegen_capture_state.width && (g_framegen_capture_state.width!=canvas.width || g_framegen_capture_state.height!=canvas.height || g_framegen_capture_state.format!=canvas.pixelFormat)) {
        framegen_resize_wait("canvas",g_framegen_capture_state.width,g_framegen_capture_state.height,canvas.width,canvas.height);
    }
    id<MTLTexture> scene=dm_fg_scene_target(g_dev,canvas.width,canvas.height,canvas.pixelFormat);
    static int resident;
    if (!resident) { [g_q4 addResidencySet:dm_fg_residency()]; resident=1; }
    enc_b_copy_texture(blit(),canvas,0,0,MTLOriginMake(0,0,0),MTLSizeMake(canvas.width,canvas.height,1),scene,0,0,MTLOriginMake(0,0,0));
    if (g_blit) { enc_b_end(g_blit); g_blit=nil; }
    g_framegen_capture_state.composited_canvas=canvas;
    g_framegen_capture_state.width=canvas.width; g_framegen_capture_state.height=canvas.height; g_framegen_capture_state.format=canvas.pixelFormat;
    dm_fg_note_scene();
    if (dm_fg_profile_on()) dm_fg_profile_event(@"scene_capture",@{@"frame":@(atomic_load(&g_cur_frame)),@"w":@(canvas.width),@"h":@(canvas.height),@"depth_w":@(g_framegen_capture_state.depth_width),@"depth_h":@(g_framegen_capture_state.depth_height),@"canvas_serial":@(R.rt.color[0]->serial),@"fs":@(program->sh[1]->key)});
}
static void framegen_copy_composited_profile(id<MTL4CommandBuffer> cb,id<MTL4CounterHeap> heap,NSUInteger base) {
    id<MTLTexture> canvas=g_framegen_capture_state.composited_canvas;
    id<MTLTexture> ui=dm_fg_ui_target(g_dev,canvas.width,canvas.height);
    id<MTL4ComputeCommandEncoder> encoder=[cb computeCommandEncoder];
    [encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
    if (heap) {
        [encoder endEncoding]; [cb writeTimestampIntoHeap:heap atIndex:base+1];
        encoder=[cb computeCommandEncoder];
        [encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
    }
    [encoder copyFromTexture:canvas sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(canvas.width,canvas.height,1) toTexture:ui destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [encoder barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [encoder endEncoding];
    if (heap) [cb writeTimestampIntoHeap:heap atIndex:base+2];
}
static void framegen_copy_composited(id<MTL4CommandBuffer> cb) { framegen_copy_composited_profile(cb,nil,0); }
static int framegen_generate(double period,int interpolate) {
    // QueuePresentTexture is outside a game submission. Finish deferred Metal
    // encoding, then place the UI copy and MetalFX behind the frame's own work.
    static id<MTL4CommandAllocator> allocators[3];
    static id<MTL4CommandBuffer> buffers[3];
    static id<MTL4CounterHeap> profile_heap; static double profile_hz;
    // The classic/rescue presenter displays real frames only. Do not capture
    // or interpolate outputs it cannot consume; the next active pair is fresh.
    if (!framegen_present_active()) { framegen_discard_inputs(); return -1; }
    int slot=-1;
    if (dm_fg_available(g_dev)) {
        if (g_framegen_capture_state.invalid) dm_fg_reset();
        else {
            slot=dm_fg_frame_prepare(period,interpolate);
            if (slot>=0) {
                int profile=dm_fg_profile_on(); double cpu_begin=profile?CACurrentMediaTime():0;
                rec_drain(); double drained=profile?CACurrentMediaTime():0;
                up_flush(); res_commit();
                NSDictionary *context=profile?dm_fg_profile_frame():nil;
                if (profile && !profile_heap) {
                    MTL4CounterHeapDescriptor *d=[MTL4CounterHeapDescriptor new]; d.type=MTL4CounterHeapTypeTimestamp; d.count=18;
                    NSError *error=nil; profile_heap=[g_dev newCounterHeapWithDescriptor:d error:&error]; profile_hz=[g_dev queryTimestampFrequency];
                    if (!profile_heap || !profile_hz) MDIE("frame generation profiling counters: %s",error.localizedDescription.UTF8String);
                }
                if (!allocators[slot]) { allocators[slot]=[g_dev newCommandAllocator]; buffers[slot]=[g_dev newCommandBuffer]; }
                if (!allocators[slot] || !buffers[slot]) MDIE("frame generation command allocation failed");
                [allocators[slot] reset];
                id<MTL4CommandBuffer> cb=buffers[slot];
                [cb beginCommandBufferWithAllocator:allocators[slot]];
                NSUInteger base=6*slot;
                if (profile) [cb writeTimestampIntoHeap:profile_heap atIndex:base];
                framegen_copy_composited_profile(cb,profile?profile_heap:nil,base);
                NSArray *resources=dm_fg_frame_encode(cb,slot,profile?profile_heap:nil,base+2);
                id<MTLTexture> canvas=g_framegen_capture_state.composited_canvas;
                [cb endCommandBuffer];
                MTL4CommitOptions *options=[MTL4CommitOptions new];
                uint64_t frame=atomic_load(&g_cur_frame); gpu_tag_begin(frame);
                double lock_begin=profile?CACurrentMediaTime():0;
                pthread_mutex_lock(&g_res_mu);
                double commit_s=profile?CACurrentMediaTime():0;
                [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
                    (void)resources.count; (void)canvas.width;
                    if (feedback.error) MLOG("[RED] frame generation: %s",feedback.error.description.UTF8String);
                    gpu_tag_done(frame,feedback.GPUStartTime,feedback.GPUEndTime);
                    dm_fg_gpu_time(feedback.GPUEndTime-feedback.GPUStartTime);
                    if (profile) {
                        double callback_s=CACurrentMediaTime();
                        NSData *raw=[profile_heap resolveCounterRange:NSMakeRange(base,6)];
                        const MTL4TimestampHeapEntry *t=raw.bytes;
                        int valid=!feedback.error && raw.length>=6*sizeof *t;
                        if (valid) for (int i=1;i<6;i++) valid &= t[i].timestamp>=t[i-1].timestamp && t[i-1].timestamp>0;
                        NSMutableDictionary *row=[context mutableCopy];
                        if (raw.length>=6*sizeof *t) { NSMutableArray *ticks=[NSMutableArray new]; for (int i=0;i<6;i++) [ticks addObject:@(t[i].timestamp)]; row[@"gpu_ticks"]=ticks; }
                        row[@"frame"]=@(frame); row[@"slot"]=@(slot); row[@"submit_s"]=@(commit_s); row[@"gpu_start_s"]=@(feedback.GPUStartTime); row[@"gpu_end_s"]=@(feedback.GPUEndTime);
                        row[@"submit_to_gpu_ms"]=@((feedback.GPUStartTime-commit_s)*1000); row[@"gpu_span_ms"]=@((feedback.GPUEndTime-feedback.GPUStartTime)*1000);
                        row[@"feedback_lag_ms"]=@((callback_s-feedback.GPUEndTime)*1000); row[@"cpu_drain_ms"]=@((drained-cpu_begin)*1000); row[@"cpu_encode_ms"]=@((lock_begin-drained)*1000); row[@"cpu_submit_lock_ms"]=@((commit_s-lock_begin)*1000); row[@"counters_valid"]=@(valid);
                        if (valid) {
                            NSArray *names=@[@"gpu_pre_copy_ms",@"ui_copy_and_dependency_ms",@"motion_ms",@"previous_depth_copy_ms",@"metalfx_ms"];
                            for (int i=0;i<5;i++) row[names[i]]=@((t[i+1].timestamp-t[i].timestamp)*1000.0/profile_hz);
                        }
                        dm_fg_profile_event(@"gpu_frame",row);
                    }
                    dm_fg_generation_done(slot);
                }];
                [g_q4 commit:&cb count:1 options:options]; pthread_mutex_unlock(&g_res_mu);
            }
        }
        if (++g_framegen_capture_state.frames%300==0) { char stats[256]; dm_fg_stats(stats,sizeof stats); MLOG("[framegen] real frames=%llu %s",(unsigned long long)g_framegen_capture_state.frames,stats); }
        g_framegen_capture_state.depth=g_framegen_capture_state.want_camera=g_framegen_capture_state.invalid=0;
        g_framegen_capture_state.composited_canvas=nil;
    }
    return slot;
}
