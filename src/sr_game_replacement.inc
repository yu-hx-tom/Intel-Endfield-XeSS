// Included in GameSrRuntime. No separate scheduler, submissions, waits or worker.
    void replacementReport(const char* reason) noexcept {
        const auto& c=nativeTransaction.counts();JsonLine line("sr_native_replacement");line.text("reason",reason);
        line.number("schema",3);line.text("recording_scope","application_command_recorded_not_gpu_complete");line.text("policy","xess_or_engine_native_no_fsr_core");
        line.text("output_mode",outputMode);line.number("degraded_since_frame",degradedFrame);line.number("degraded_since_qpc",degradedQpc);
        line.number("application_dispatch_attempts_in_core_region",applicationDispatches);line.number("application_dispatch_forwarded_in_core_region",applicationForwarded);
        line.number("application_dispatch_suppressed_in_core_region",applicationSuppressed);line.number("unknown_dispatches_in_core_region",unknownDispatches);
        line.text("dispatch_scope","prepare_through_accumulate_excluding_sdk_injected_commands");
        line.text("known_seven_core_fingerprint","712fec73dda0fb37a4381e26b9be9dd22b96c6a17fdc2e0685b020d57e3ed2e7");
        line.number("replacement_frames",c.replacementFrames);line.number("native_fsr_frames",0);
        line.number("guard_triggers",c.guards);line.number("full_discards",c.discards);line.number("capacity_failures",c.capacity);
        line.number("transition_blits",replacementBlits);
        line.number("replacement_xess_executed",replacementExecuteCount);
        line.number("photo_full_writes_recorded",replacementPhotoFullWrites);line.number("photo_full_writes_skipped",replacementPhotoFullWritesSkipped);line.number("photo_coverage_refusals",replacementPhotoCoverageRefusals);
        line.number("auxiliary_full_writes_recorded",replacementAuxFullWrites);line.number("auxiliary_full_writes_skipped",replacementAuxFullWritesSkipped);line.number("auxiliary_coverage_refusals",replacementAuxCoverageRefusals);
        line.number("non_reading_compute_references",replacementNonReadingReferences);
        line.number("watched_non_reading_compute_references",replacementWatchedNonReadingReferences);
        line.number("undefined_layout_omitted_discards",replacementUndefinedLayoutDiscards);
        line.number("full_write_retired_while_suppressed",fullWriteRetiredWhileSuppressed);
        line.number("missing_core_contents",nativeTransaction.missingContents());
        line.cpuWall("cpu_replacement_commands",replacementCommandCpu);
        line.boolean("cpu_replacement_commands_timing_enabled",trackingCpuTimingEnabled());
        line.number("replacement_command_calls",replacementCommandCalls);
        line.number("graphics_draw_checks",replacementDrawChecks);line.number("graphics_bind_checks",replacementBindChecks);
        line.number("descriptor_set_reference_queries",replacementDescriptorQueries);line.number("descriptor_reference_batches",replacementDescriptorBatches);
        line.number("descriptor_set_identity_queries",replacementIdentityQueries);line.number("descriptor_identity_batches",replacementIdentityBatches);
        line.number("graphics_safe_cache_hits",replacementSafeCacheHits);line.number("explicit_omitted_lookups",replacementOmittedLookups);
        line.number("descriptor_identity_cache_hits",replacementIdentityCacheHits);line.number("descriptor_identity_cache_misses",replacementIdentityCacheMisses);
        line.number("guard_schema",2);line.number("mask_draws",replacementMaskDraws);line.number("mask_fallbacks",replacementMaskFallbacks);
        line.number("mask_refreshes",replacementMaskRefreshes);line.number("mask_configurations",replacementMaskConfigurations);
        line.text("cpu_replacement_commands_scope","guard_including_mutex_wait_excluding_tail_capture");
        line.number("fault",unsigned(nativeTransaction.fault()));line.boolean("stopped",nativeTransaction.stopped());
        line.boolean("frame_replacing",nativeTransaction.replacement());line.boolean("current_frame_complete",!nativeTransaction.open());
        line.text("terminal_reason",replacementTerminalReason);line.text("terminal_status",sro::name(replacementTerminalResult.status));
        line.text("pre_execute_fallback_refusal",replacementFallbackRefusal);
        line.key("core_dispatch_skipped");line.raw("[");for(unsigned n=0;n<7;++n){if(n)line.raw(",");char v[32];std::snprintf(v,sizeof(v),"%llu",static_cast<unsigned long long>(c.skipped[n]));line.raw(v);}line.raw("]");
        line.key("core_dispatch_forwarded");line.raw("[");for(unsigned n=0;n<7;++n){if(n)line.raw(",");char v[32];std::snprintf(v,sizeof(v),"%llu",static_cast<unsigned long long>(c.forwarded[n]));line.raw(v);}line.raw("]");
        line.number("xess_executed_total",executed);line.writeObservation();
        if(diagnosticLoggingEnabled()&&std::string_view(lastReplacementFault.origin)!="none"){
            const auto& replacementFault=lastReplacementFault;
            JsonLine fault("sr_native_replacement_fault");fault.text("origin",replacementFault.origin);
            fault.number("fault",unsigned(lastReplacementFaultCode));fault.number("issues",replacementFault.issues);
            fault.boolean("currently_stopped",nativeTransaction.stopped());fault.boolean("retained_last_fault",true);
            fault.handle("descriptor_set",replacementFault.descriptorSet.handle);fault.number("descriptor_set_generation",replacementFault.descriptorSet.generation);
            fault.number("set_index",replacementFault.setIndex);fault.number("bind_capture_issues",replacementFault.bindIssues);
            fault.handle("descriptor_layout",replacementFault.descriptorLayout.handle);fault.number("descriptor_layout_generation",replacementFault.descriptorLayout.generation);
            fault.number("set_issues",replacementFault.setIssues);fault.number("layout_issues",replacementFault.layoutIssues);
            fault.number("declared_image_descriptors",replacementFault.declaredImages);fault.number("layout_bindings",replacementFault.layoutBindings);
            fault.number("unwritten_image_descriptors",replacementFault.unwrittenImages);fault.number("image_metadata_capacity",xess_vk::descriptor_image_metadata::maxSetImageBindings);
            fault.handle("command",replacementFault.recording.command);fault.number("recording",replacementFault.recording.rpRecordingGeneration);
            fault.number("order",replacementFault.recording.commonRecordOrder);fault.handle("pipeline",replacementFault.pipeline.handle);
            fault.number("pipeline_generation",replacementFault.pipeline.generation);fault.handle("image",replacementFault.image.handle);
            const auto consumerPipeline=reinterpret_cast<VkPipeline>(replacementFault.pipeline.handle);
            const auto digest=xess_vk::compute_observer::archivedShaderFingerprint(device->srShaders,consumerPipeline,replacementFault.pipeline.generation);
            std::array<char,65> digestText{};std::copy(digest.begin(),digest.end(),digestText.begin());fault.text("consumer_shader_sha256",digestText.data());
            const auto access=xess_vk::compute_observer::archivedImageReads(device->srShaders,consumerPipeline,replacementFault.pipeline.generation);
            fault.boolean("consumer_access_known",access.known);fault.boolean("consumer_binding_may_read",access.mayRead(replacementFault.setIndex,replacementFault.binding));
            fault.number("image_generation",replacementFault.image.generation);fault.number("load_op",replacementFault.load);
            fault.boolean("content_omitted",replacementFault.contentOmitted);fault.number("producer_profile",replacementFault.producerProfile);
            fault.number("missing_core_contents",replacementFault.missingContents);fault.boolean("frame_open",replacementFault.frameOpen);
            fault.boolean("frame_replacing",replacementFault.frameReplacing);fault.number("binding",replacementFault.binding);
            fault.number("array_element",replacementFault.arrayElement);fault.number("descriptor_type",replacementFault.descriptorType);
            fault.boolean("attachment_known",replacementFault.attachmentKnown);fault.number("image_width",replacementFault.width);fault.number("image_height",replacementFault.height);
            fault.number("area_x",replacementFault.area.offset.x);fault.number("area_y",replacementFault.area.offset.y);
            fault.number("area_width",replacementFault.area.extent.width);fault.number("area_height",replacementFault.area.extent.height);fault.number("layers",replacementFault.layers);
            fault.number("aspect",replacementFault.range.aspectMask);fault.number("base_mip",replacementFault.range.baseMipLevel);fault.number("mip_count",replacementFault.range.levelCount);
            fault.number("base_layer",replacementFault.range.baseArrayLayer);fault.number("layer_count",replacementFault.range.layerCount);
            // Explicit release diagnostics need the actual refusal, not only the later held-output state.
            fault.write();
        }
        JsonLine gpu("sr_native_replacement_gpu_totals");gpu.text("reason",reason);
        constexpr const char* names[]{"native_fsr","xess","suppressed_region"};
        for(unsigned n=0;n<3;++n){gpu.number(names[n],gpuSampleCount[n]);}
        gpu.key("total_us");gpu.raw("[");for(unsigned n=0;n<3;++n){if(n)gpu.raw(",");char v[48];std::snprintf(v,sizeof(v),"%.3f",gpuSampleNs[n]/1000);gpu.raw(v);}gpu.raw("]");gpu.writeObservation();
    }
    void replacementFatalIfNeeded() noexcept {
        if(!nativeTransaction.fatal())return;
        if(!replacementFaultHandled){
            replacementFaultHandled=true;
            // Only a consumer fault names a pipeline whose silence permits a return.
            fallbackHold("held_unproven_consumer",nativeTransaction.fault()==xess_vk::sr_replacement::Fault::Consumer?
                lastReplacementFault.pipeline:xess_vk::descriptor_image_metadata::Key{});
            degrade(replacementPlanFrame.engineFrame,"held_unproven_consumer");classify(replacementPlanFrame.engineFrame,2);
            xess_vk::sr_native_resolution::control.request(xess_vk::sr_native_resolution::Reason::ConsumerUnproven);
            replacementReport("output_held_engine_native_requested");
        }
        if(xess_vk::diagnostic_flags::srFatalOnUnproven())TerminateProcess(GetCurrentProcess(),0xE0550002u);
    }
    void replacementStop(xess_vk::sr_replacement::Fault fault,const char* origin,const xess_vk::CommandRecordingFacts& f={},
                         std::uint64_t issues=0,xess_vk::descriptor_image_metadata::Key pipeline={}) noexcept {
        if(nativeTransaction.stopped())return;
        issue(replacementPlanFrame.engineFrame,origin);
        replacementFault.origin=origin;replacementFault.recording=f;replacementFault.issues=issues;replacementFault.pipeline=pipeline;
        lastReplacementFault=replacementFault;lastReplacementFaultCode=fault;
        nativeTransaction.stop(fault);if(nativeTransaction.counts().guards<=2||nativeTransaction.fatal())replacementReport("guard_stopped");
    }
    const PipelineProfile& replacementPipeline(const xess_vk::descriptor_image_metadata::DispatchEvidence& e) noexcept {
        for(const auto& p:replacementPipelines)if(p.key==e.pipeline&&e.pipeline.handle)return p;
        int result=-1;
        for(unsigned n=0;n<xess_vk::sr_native_shaders::profiles.size();++n)
            if(xess_vk::compute_observer::archivedShaderMatches(device->srShaders,reinterpret_cast<VkPipeline>(e.pipeline.handle),e.pipeline.generation,
                xess_vk::sr_native_shaders::profiles[n].sha256)){result=int(n);break;}
        if(result<0&&xess_vk::compute_observer::archivedShaderMatches(device->srShaders,reinterpret_cast<VkPipeline>(e.pipeline.handle),e.pipeline.generation,
            xess_vk::sr_native_shaders::auxiliaryWriterSha256))result=xess_vk::sr_native_shaders::auxiliaryWriterProfile;
        if(result<0&&xess_vk::compute_observer::archivedShaderMatches(device->srShaders,reinterpret_cast<VkPipeline>(e.pipeline.handle),e.pipeline.generation,
            xess_vk::sr_native_shaders::photoWriterSha256))result=xess_vk::sr_native_shaders::photoWriterProfile;
        if(result<0&&xess_vk::compute_observer::archivedShaderMatches(device->srShaders,reinterpret_cast<VkPipeline>(e.pipeline.handle),e.pipeline.generation,
            xess_vk::sr_native_shaders::characterMenuWriterSha256))result=xess_vk::sr_native_shaders::characterMenuWriterProfile;
        const bool characterMenu=result==xess_vk::sr_native_shaders::characterMenuWriterProfile||(result<0&&
            xess_vk::compute_observer::archivedShaderMatches(device->srShaders,reinterpret_cast<VkPipeline>(e.pipeline.handle),
                e.pipeline.generation,xess_vk::sr_native_shaders::characterMenuReaderSha256));
        auto& cached=replacementPipelines[replacementPipelineSerial++%replacementPipelines.size()];
        cached={e.pipeline,result,xess_vk::compute_observer::archivedImageReads(device->srShaders,
            reinterpret_cast<VkPipeline>(e.pipeline.handle),e.pipeline.generation),characterMenu};
        return cached;
    }
    bool replacementColorFallback(const xess_vk::CommandRecordingFacts& f,const xess_vk::descriptor_image_metadata::DispatchEvidence& e,
                                  const xess_vk::FrameTimingAssociation& frame) noexcept {
        replacementFallbackRefusal="frame_or_plan";
        if(!nativeTransaction.replacement()||replacementTerminalResult.status==sro::Status::SdkFailureCommandsUncertain||
           !e.metadataAccepted||e.issues||!replacementPlanFrame.valid||!frame.valid||
           frame.engineFrame!=replacementPlanFrame.engineFrame||frame.ticket!=replacementPlanFrame.ticket||
           !nativeTransaction.owns(f.command,f.rpRecordingGeneration))return false;
        const xess_vk::descriptor_image_metadata::ImageBinding *color=nullptr,*output=nullptr;
        for(std::size_t n=0;n<e.count;++n){const auto& b=e.bindings[n];if(b.setIndex)continue;
            if(b.binding==3&&b.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)color=&b;
            if(b.binding==1&&b.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)output=&b;
        }
        replacementFallbackRefusal="current_color_pair";if(!color||!output)return false;
        replacementFallbackRefusal="color_source_omitted";if(nativeTransaction.omitted(color->image))return false;
        sro::Frame safe;safe.recording=f;safe.owner={f.device,f.contextIncarnation,f.command,f.rpRecordingGeneration,f.commonRecordOrder,frame.engineFrame};
        safe.configuration.input={color->imageDescription.width,color->imageDescription.height};
        safe.configuration.output={output->imageDescription.width,output->imageDescription.height};
        safe.configuration.domain=sro::ColorDomain::SceneLinearHdr;safe.configuration.invertedDepth=true;
        safe.configuration.velocityScaleX=safe.configuration.velocityScaleY=1.f;
        replacementFallbackRefusal="color_pair_lifetime";
        if(!image(*color,0,safe.owner,safe.color)||!image(*output,3,safe.owner,safe.nativeOutput))return false;
        const auto layouts=xess_vk::render_pass_observer::sourceLayoutQuery(device->renderPass);
        replacementFallbackRefusal="color_pair_layout";
        for(const auto* b:{color,output}){
            if(!layouts.resolve)continue;
            const auto layout=layouts.resolve(layouts.user,f,{b->image.handle,b->image.generation},b->range,b->declaredLayout);
            if(layout.issues&xess_vk::bit(xess_vk::RecordedLayoutIssue::DeclaredLayoutMismatch))return false;
        }
        // The pinned Accumulate actually reads b3 and writes b1. Their current
        // typed descriptor layouts are its valid-use contract; no SDK commands
        // have changed them. Missing attachment-only history cannot invalidate
        // this transfer contract, while an observed contradiction still refuses.
        safe.pinnedModule=safe.nativeCoreReplaced=safe.wholeCoreTransactionVerified=safe.atAccumulateBoundary=safe.sameFrameInputs=true;
        safe.domainAndDownstreamVerified=safe.applicationReferencesRetained=safe.aliasCoverageComplete=safe.sourcesUnchangedAtUse=true;
        sro::VulkanFunctions vk;vk.barrier=device->cmdPipelineBarrier;vk.blit=device->cmdBlitImage;vk.enterInjectedRecording=enter;vk.leaveInjectedRecording=leave;vk.user=this;
        const auto result=sro::recordPreExecuteColorFallback(safe,vk);
        replacementFallbackRefusal=sro::name(result.status);
        if(!result.fallbackRecorded)return false;
        ++replacementBlits;replacementExecutedFrame=frame.engineFrame;replacementReset=true;
        // Permit a short colour-only transition for initialization/rebuild and
        // an isolated invalid temporal sample. Never run FSR during that window.
        classify(frame.engineFrame,1);outputMode=executed?"color_transition":"initializing";
        issue(frame.engineFrame,replacementTerminalReason);
        // Initial SDK build/rebuild has no steady output to lose. It is not a steady-state failure streak.
        if(executed&&desiredKnown&&desired.input.width==safe.configuration.input.width&&desired.input.height==safe.configuration.input.height&&
           desired.output.width==safe.configuration.output.width&&desired.output.height==safe.configuration.output.height&&
           overlay.availability(desired,key(f),frame.engineFrame)==sro::Status::Ready){
            degrade(frame.engineFrame,"color_transition");
            if(++consecutiveTransitions>=3)xess_vk::sr_native_resolution::control.request(xess_vk::sr_native_resolution::Reason::InputUnavailable);
        }else consecutiveTransitions=0;
        holdOutput=false;replacementFallbackRefusal="none";
        nativeTransaction.use(output->image,true);
        JsonLine line("sr_native_replacement_pre_execute_blit");line.text("rejected_reason",replacementTerminalReason);
        line.text("rejected_status",sro::name(replacementTerminalResult.status));line.number("frame",frame.engineFrame);
        line.handle("color",color->image.handle);line.handle("output",output->image.handle);line.boolean("sdk_commands_recorded",false);line.writeObservation();
        return true;
    }
    // nullptr when the write provably covers the whole image; otherwise the first failed condition.
    static const char* replacementFullWriteRefusal(const xess_vk::descriptor_image_metadata::ImageBinding& b,
                                                   const xess_vk::descriptor_image_metadata::DispatchEvidence& e,bool auxiliary,bool mask=false) noexcept {
        const auto& d=b.imageDescription;
        // The pinned writers use GlobalInvocationId.xy with
        // local size 8x8x1. Only a complete view and zero-based grid kill old content.
        if(!b.view.handle||!b.view.generation)return "view_unknown";
        if(b.arrayElement||b.viewType!=VK_IMAGE_VIEW_TYPE_2D)return "view_type";
        if(b.viewFormat!=d.format)return "view_format";
        if(mask?d.format!=VK_FORMAT_R8_UNORM:
           !(d.format==VK_FORMAT_R16_SFLOAT||(auxiliary&&d.format==VK_FORMAT_R16G16B16A16_SFLOAT)))return "image_format";
        if(b.declaredLayout!=VK_IMAGE_LAYOUT_GENERAL)return "layout";
        if(b.components.r!=VK_COMPONENT_SWIZZLE_IDENTITY||b.components.g!=VK_COMPONENT_SWIZZLE_IDENTITY||
           b.components.b!=VK_COMPONENT_SWIZZLE_IDENTITY||b.components.a!=VK_COMPONENT_SWIZZLE_IDENTITY)return "swizzle";
        if(!d.width||!d.height||d.depth!=1||d.mipLevels!=1||d.arrayLayers!=1||d.samples!=VK_SAMPLE_COUNT_1_BIT||d.flags)return "image_shape";
        if(!(d.usage&VK_IMAGE_USAGE_STORAGE_BIT))return "usage";
        if(!xess_vk::sr_replacement::fullColorRange(b.range,d.mipLevels,d.arrayLayers))return "view_range";
        if(e.groups[0]!=(std::uint64_t(d.width)+7)/8||e.groups[1]!=(std::uint64_t(d.height)+7)/8||e.groups[2]!=1)return "grid";
        return nullptr;
    }
    static bool replacementKnownFullWrite(const xess_vk::descriptor_image_metadata::ImageBinding& b,
                                         const xess_vk::descriptor_image_metadata::DispatchEvidence& e,bool auxiliary,bool mask=false) noexcept {
        return !replacementFullWriteRefusal(b,e,auxiliary,mask);
    }
    void replacementWatchedUse(const xess_vk::descriptor_image_metadata::ImageBinding& b,const xess_vk::descriptor_image_metadata::DispatchEvidence& e,
                               const PipelineProfile& pipeline,bool writeOnly,bool notRead,bool direct,const xess_vk::CommandRecordingFacts& f) noexcept {
        for(unsigned n=0;n<replacementWatchedUseCount;++n){const auto& u=replacementWatchedUses[n];
            if(u.pipeline==e.pipeline&&u.set==b.setIndex&&u.binding==b.binding)return;}
        if(replacementWatchedUseCount>=replacementWatchedUses.size())return;
        replacementWatchedUses[replacementWatchedUseCount++]={e.pipeline,b.setIndex,b.binding};
        const auto& d=b.imageDescription;const auto handle=reinterpret_cast<VkPipeline>(e.pipeline.handle);
        const auto digest=xess_vk::compute_observer::archivedShaderFingerprint(device->srShaders,handle,e.pipeline.generation);
        std::array<char,65> digestText{};std::copy(digest.begin(),digest.end(),digestText.begin());
        JsonLine line("sr_watched_image_use");line.number("schema",1);line.number("first_uses",replacementWatchedUseCount);
        line.handle("pipeline",e.pipeline.handle);line.number("pipeline_generation",e.pipeline.generation);line.text("shader_sha256",digestText.data());
        line.number("profile",pipeline.profile);line.boolean("access_known",pipeline.reads.known);line.boolean("may_read",pipeline.reads.mayRead(b.setIndex,b.binding));
        line.boolean("write_only",writeOnly);line.boolean("not_read",notRead);line.boolean("direct",direct);
        line.boolean("metadata_accepted",e.metadataAccepted);line.number("evidence_issues",e.issues);line.boolean("membership_only",e.membershipOnly);
        line.number("set_index",b.setIndex);line.number("binding",b.binding);line.number("array_element",b.arrayElement);line.number("descriptor_type",b.descriptorType);
        line.handle("image",b.image.handle);line.number("image_generation",b.image.generation);
        line.boolean("content_omitted",nativeTransaction.omitted(b.image));line.number("producer_profile",nativeTransaction.producer(b.image));
        line.number("image_width",d.width);line.number("image_height",d.height);line.number("image_format",d.format);line.number("image_usage",d.usage);
        line.number("mip_levels",d.mipLevels);line.number("array_layers",d.arrayLayers);line.number("view_type",b.viewType);line.number("view_format",b.viewFormat);
        line.number("base_mip",b.range.baseMipLevel);line.number("mip_count",b.range.levelCount);line.number("base_layer",b.range.baseArrayLayer);line.number("layer_count",b.range.layerCount);
        line.number("declared_layout",b.declaredLayout);
        line.number("groups_x",e.groups[0]);line.number("groups_y",e.groups[1]);line.number("groups_z",e.groups[2]);
        const char* refusal=replacementFullWriteRefusal(b,e,pipeline.profile==xess_vk::sr_native_shaders::auxiliaryWriterProfile,pipeline.profile==1);
        line.text("full_write_refusal",refusal?refusal:"none");
        line.handle("command",f.command);line.number("recording",f.rpRecordingGeneration);line.number("order",f.commonRecordOrder);
        line.boolean("transaction_open",nativeTransaction.open());line.boolean("transaction_stopped",nativeTransaction.stopped());
        line.number("missing_core_contents",nativeTransaction.missingContents());line.number("presents",presents);line.write();
    }
    void replacementPrepareOutputs(const xess_vk::descriptor_image_metadata::DispatchEvidence& e,const xess_vk::CommandRecordingFacts& f) noexcept {
        std::uint64_t hash=1469598103934665603ull;
        for(std::size_t n=0;n<e.count;++n){const auto& b=e.bindings[n];if(b.setIndex||b.descriptorType!=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)continue;
            hash=(hash^b.image.handle^(b.image.generation<<40)^b.binding)*1099511628211ull;}
        if(hash==replacementPrepareOutputsHash||replacementPrepareOutputReports>=16)return;
        replacementPrepareOutputsHash=hash;++replacementPrepareOutputReports;
        JsonLine line("sr_prepare_outputs");line.number("schema",1);line.number("report",replacementPrepareOutputReports);
        line.handle("command",f.command);line.number("recording",f.rpRecordingGeneration);line.number("order",f.commonRecordOrder);
        line.number("groups_x",e.groups[0]);line.number("groups_y",e.groups[1]);line.number("presents",presents);line.key("bindings");line.raw("[");
        bool first=true;
        for(std::size_t n=0;n<e.count;++n){const auto& b=e.bindings[n];if(b.setIndex)continue;const auto& d=b.imageDescription;
            char item[256];std::snprintf(item,sizeof(item),"%s{\"binding\":%u,\"type\":%u,\"image\":\"0x%llx\",\"generation\":%llu,\"width\":%u,\"height\":%u,\"format\":%u}",
                first?"":",",unsigned(b.binding),unsigned(b.descriptorType),static_cast<unsigned long long>(b.image.handle),
                static_cast<unsigned long long>(b.image.generation),unsigned(d.width),unsigned(d.height),unsigned(d.format));
            line.raw(item);first=false;}
        line.raw("]");line.write();
    }
    bool intercept(const xess_vk::CommandRecordingFacts& f,const xess_vk::descriptor_image_metadata::DispatchEvidence& e,
                   const xess_vk::sr_observation::DispatchTicket& t,bool direct) noexcept {
        if(!replacementAvailable||srSdkDepth)return false;
        bool suppress=false,terminal=false,commandUnsafe=false;
        const auto frame=t.frame.valid?t.frame:renderTailFrame(device);
        {
            std::lock_guard lock(mutex);const auto& pipeline=replacementPipeline(e);int profile=pipeline.profile;
            // Its gameplay state stays active, so only the menu's own passes can keep FG off there.
            if(pipeline.characterMenu)xess_vk::fsr_menu_control::characterMenuObserved();
            if(fallbackTransparent()){
                // The engine's own frames: forward everything and only observe an
                // upscaling Accumulate and the consumer pipeline that caused the stop.
                if(profile==int(xess_vk::sr_replacement::lastCore))fallbackUpscaleAt=presents;
                const auto consumers=fallbackConsumers.begin()+fallbackConsumerCount;
                if(e.pipeline.handle&&std::find(fallbackConsumers.begin(),consumers,e.pipeline)!=consumers)fallbackConsumerAt=presents;
                ++fallbackForwarded;return false;
            }
            const bool unsupportedAA=profile<0&&t.containsAccumulate&&t.extentsKnown&&t.frame.valid&&
                t.pipeline==e.pipeline.handle&&t.pipelineGeneration==e.pipeline.generation&&
                t.renderExtent.width&&t.renderExtent.height&&t.renderExtent.width==t.outputExtent.width&&t.renderExtent.height==t.outputExtent.height;
            if(unsupportedAA){
                // A Native-AA pass that exists only because native rendering was requested is the
                // fallback image itself, not a consumer whose silence must precede a return.
                const bool engineNativeRequested=xess_vk::sr_native_resolution::control.requested();
                replacementStop(xess_vk::sr_replacement::Fault::Sequence,"native_aa_shader_unqualified",f,0,e.pipeline);
                // Render extent == output extent is the engine's own native frame.
                commandUnsafe=true;
                fallbackHold("native_aa_unqualified",engineNativeRequested?xess_vk::descriptor_image_metadata::Key{}:e.pipeline);fallbackNativeSeen=true;
                classify(frame.engineFrame,2);degrade(frame.engineFrame,"native_aa_unqualified");
                xess_vk::sr_native_resolution::control.request(xess_vk::sr_native_resolution::Reason::RuntimeUnavailable);
                // A candidate may request evidence or refuse work; it never grants a replacement profile.
                if(++nativeAaUnqualified==1){JsonLine line("sr_native_aa_unqualified");line.number("schema",3);line.boolean("replacement_permitted",false);
                    line.handle("pipeline",e.pipeline.handle);line.number("pipeline_generation",e.pipeline.generation);line.writeObservation();
                    xess_vk::compute_observer::selectArchivedShader(device->srShaders,reinterpret_cast<VkPipeline>(e.pipeline.handle),e.pipeline.generation,replacementArchiveLine);}
            }
            if(profile==2){
                countingCoreRegion=true;
                if(auto* owner=guard(f))owner->coreOmitted=true;
                else {recoveryTrackingLost=true;replacementStop(xess_vk::sr_replacement::Fault::Capacity,"recording_owner_untracked",f);}
                nativeTransaction.begin(frame.engineFrame,f.command,f.rpRecordingGeneration);
                replacementPlanFrame=frame;
                if(!direct||!profileReady()||!xess_vk::sr_native_resolution::control.installed.load()||f.queueFamily!=device->srQueueFamily||!f.scopeKnown||
                   f.inRenderPass||f.inDynamicRendering||f.reasons||f.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY)
                    replacementStop(xess_vk::sr_replacement::Fault::Sequence,"prepare_recording_contract",f);
                if(replacementPrepareReports++<3)replacementReport("prepare_core_suppressed");
                if(diagnosticLoggingEnabled())replacementPrepareOutputs(e,f);
            }
            const bool inCoreRegion=countingCoreRegion;
            if(inCoreRegion){++applicationDispatches;if(pipeline.profile<0)++unknownDispatches;}
            const bool internal=profile>=2&&profile<=8;
            if(!internal){
                std::array<const xess_vk::descriptor_image_metadata::ImageBinding*,2> fullDestinations{};unsigned fullDestinationCount{};
                const bool auxiliary=profile==xess_vk::sr_native_shaders::auxiliaryWriterProfile;
                const bool mask=profile==1;
                for(std::size_t n=0;n<e.guardImageCount;++n){const auto& b=e.bindings[n];
                    // Exact pinned kept writers: set 0/b0 has OpImageWrite,
                    // no read. Keep the old-content watch until a full discard.
                    const bool keptWriter=profile==0||mask||profile==xess_vk::sr_native_shaders::auxiliaryWriterProfile||
                        profile==xess_vk::sr_native_shaders::photoWriterProfile||profile==xess_vk::sr_native_shaders::characterMenuWriterProfile;
                    const bool writeOnly=keptWriter&&direct&&e.metadataAccepted&&!e.issues&&b.setIndex==0&&(b.binding==0||(mask&&b.binding==1))&&
                        b.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    // A bound descriptor is not necessarily read by this shader.
                    // This does not discard its old content or prove a full write.
                    const bool notRead=direct&&e.guardImagesKnown&&!e.issues&&!pipeline.reads.mayRead(b.setIndex,b.binding);
                    const bool photoClass=profile==xess_vk::sr_native_shaders::photoWriterProfile||profile==xess_vk::sr_native_shaders::characterMenuWriterProfile;
                    if(writeOnly&&(auxiliary||mask||photoClass)&&fullDestinationCount<fullDestinations.size())
                        fullDestinations[fullDestinationCount++]=&b;
                    if(diagnosticLoggingEnabled()&&nativeTransaction.watches(b.image))replacementWatchedUse(b,e,pipeline,writeOnly,notRead,direct,f);
                    if(notRead){
                        ++replacementNonReadingReferences;
                        if(nativeTransaction.watches(b.image)&&++replacementWatchedNonReadingReferences<=3){
                            JsonLine line("sr_non_reading_compute_reference");line.handle("pipeline",e.pipeline.handle);
                            line.number("pipeline_generation",e.pipeline.generation);line.handle("image",b.image.handle);
                            line.number("image_generation",b.image.generation);line.number("set_index",b.setIndex);line.number("binding",b.binding);
                            line.boolean("old_content_omitted",nativeTransaction.omitted(b.image));line.boolean("contents_discarded",false);
                            line.boolean("membership_only",e.membershipOnly);line.writeObservation();
                        }
                    }
                    if(!writeOnly&&!notRead){
                        commandUnsafe|=nativeTransaction.omitted(b.image);
                        if(!nativeTransaction.stopped()&&nativeTransaction.watches(b.image)){
                            replacementFault={};replacementFault.descriptorSet=b.set;replacementFault.setIndex=b.setIndex;
                            replacementFault.binding=b.binding;replacementFault.arrayElement=b.arrayElement;
                            replacementFault.descriptorType=b.descriptorType;
                            replacementFault.width=b.imageDescription.width;replacementFault.height=b.imageDescription.height;
                        }
                        replacementUse(b.image,false,"compute_dispatch",f,e.pipeline,b.set);
                    }
                }
                if(!e.guardImagesKnown&&nativeTransaction.hasContents()){commandUnsafe=true;replacementStop(xess_vk::sr_replacement::Fault::Consumer,"compute_bindings_unknown",f,e.issues,e.pipeline);}
                // Check every sampled input before retiring the destination: an
                // aliased input still needs the old content even for a full write.
                for(unsigned n=0;n<fullDestinationCount;++n){const auto* fullDestination=fullDestinations[n];
                    if(!nativeTransaction.watches(fullDestination->image))continue;
                    PendingFullWrite* pending=nullptr;
                    for(auto& w:pendingFullWrites)if(!w.recording.command){pending=&w;break;}
                    if(!pending)try{pendingFullWrites.emplace_back();pending=&pendingFullWrites.back();}
                        catch(...){replacementStop(xess_vk::sr_replacement::Fault::Capacity,"full_write_receipt_allocation_failed",f);}
                    const bool maskExtent=!mask||(t.nativePass==xess_vk::upscale_tail_probe::Pass::Transparency&&t.extentsKnown&&t.frame.valid&&
                        t.pipeline==e.pipeline.handle&&t.pipelineGeneration==e.pipeline.generation&&t.command==f.command&&t.recording==f.rpRecordingGeneration&&
                        t.groups==e.groups&&t.renderExtent.width==fullDestination->imageDescription.width&&t.renderExtent.height==fullDestination->imageDescription.height);
                    const char* refusal=maskExtent?replacementFullWriteRefusal(*fullDestination,e,auxiliary,mask):"mask_extent";
                    if(pending)*pending={f,fullDestination->image,e.pipeline,auxiliary||mask,!refusal,nativeTransaction.omitted(fullDestination->image),
                        fullDestination->imageDescription.width,fullDestination->imageDescription.height,e.groups,
                        refusal?refusal:"none",fullDestination->imageDescription.format,profile};
                }
            }
            else {
                if(!direct||!e.metadataAccepted||e.issues){suppress=true;nativeTransaction.stop(xess_vk::sr_replacement::Fault::Sequence);}
                else suppress=nativeTransaction.core(unsigned(profile),frame.engineFrame,f.command,f.rpRecordingGeneration);
                // Static write bindings were verified against these seven exact modules.
                constexpr std::array<unsigned,7> writes{0x1f,0xf,0x7f,0x1,0x7,0x3,0x7};
                for(std::size_t n=0;n<e.count;++n){const auto& b=e.bindings[n];
                    if(b.setIndex==0&&b.binding<32&&b.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE&&
                       (writes[profile-2]&(1u<<b.binding))&&!(profile==8&&b.binding==1))nativeTransaction.produce(b.image,suppress,unsigned(profile));
                }
                terminal=profile==8;
            }
            replacementFatalIfNeeded();
            if(!internal&&commandUnsafe){suppress=true;++guardSuppressedCommands;}
            if(inCoreRegion){if(suppress)++applicationSuppressed;else ++applicationForwarded;}
            if(terminal)countingCoreRegion=false;
            currentReplacement=suppress&&terminal;
            if(currentReplacement){
                terminalSuccess=false;replacementTerminalReason="terminal_ticket_missing";
                replacementTerminalResult={sro::Status::FrameUnverified};
            }
        }
        if(currentReplacement){
            if(!nativeTransaction.stopped())record(f,e,t);
            std::lock_guard lock(mutex);
            if(!terminalSuccess&&replacementExecutedFrame!=frame.engineFrame){
                if(replacementTerminalResult.status!=sro::Status::SdkFailureCommandsUncertain)
                    (void)replacementColorFallback(f,e,frame);
                if(replacementExecutedFrame!=frame.engineFrame)report("replacement_terminal_no_output",replacementTerminalResult);
            }
            nativeTransaction.complete(terminalSuccess||replacementExecutedFrame==frame.engineFrame);
            replacementFatalIfNeeded();currentReplacement=false;
        }
        return suppress;
    }
    void replacementFullWriteReturned(const xess_vk::CommandRecordingFacts& f,bool forwarded) noexcept {
        if(!replacementAvailable||srSdkDepth||!f.command||!f.rpRecordingGeneration)return;
        std::lock_guard lock(mutex);
        for(auto& entry:pendingFullWrites){
            if(entry.recording.command!=f.command||entry.recording.rpRecordingGeneration!=f.rpRecordingGeneration||
               entry.recording.commonRecordOrder!=f.commonRecordOrder)continue;
            const auto w=entry;entry={};
            // Content changes require the original writer's actual recording receipt, never just its intended coverage.
            if(w.full&&forwarded){nativeTransaction.use(w.image,true);++(w.auxiliary?replacementAuxFullWrites:replacementPhotoFullWrites);}
            else if(w.full){++(w.auxiliary?replacementAuxFullWritesSkipped:replacementPhotoFullWritesSkipped);
                fullWriteRetiredWhileSuppressed+=w.omitted&&!nativeTransaction.omitted(w.image);}
            else ++(w.auxiliary?replacementAuxCoverageRefusals:replacementPhotoCoverageRefusals);
            const auto count=w.auxiliary?replacementAuxFullWrites+replacementAuxFullWritesSkipped+replacementAuxCoverageRefusals:
                replacementPhotoFullWrites+replacementPhotoFullWritesSkipped+replacementPhotoCoverageRefusals;
            // Diagnostic mode writes these to the main log: release builds have no observation stream.
            if(count<=16&&diagnosticLoggingEnabled()){JsonLine line(w.auxiliary?"sr_auxiliary_overwrite":"sr_photo_overwrite");line.number("schema",4);
                line.boolean("full_coverage",w.full);line.text("coverage_refusal",w.refusal);line.number("format",w.format);line.number("writer_profile",w.profile);
                line.boolean("will_apply",w.full&&forwarded);line.boolean("dispatch_forwarded",forwarded);
                line.boolean("content_omitted_after",nativeTransaction.omitted(w.image));line.number("missing_core_contents",nativeTransaction.missingContents());
                line.boolean("old_content_omitted",w.omitted);line.handle("image",w.image.handle);line.number("generation",w.image.generation);
                line.handle("pipeline",w.pipeline.handle);line.number("pipeline_generation",w.pipeline.generation);
                line.handle("command",f.command);line.number("recording",f.rpRecordingGeneration);line.number("order",f.commonRecordOrder);
                line.number("width",w.width);line.number("height",w.height);line.number("groups_x",w.groups[0]);line.number("groups_y",w.groups[1]);line.number("groups_z",w.groups[2]);
                line.text("receipt_scope","application_command_recorded_not_gpu_complete");line.number("presents",presents);line.write();}
        }
    }
    void replacementEnd(const xess_vk::CommandRecordingFacts& f) noexcept {
        if(!replacementAvailable||srSdkDepth)return;std::lock_guard lock(mutex);
        if(nativeTransaction.replacement()&&nativeTransaction.owns(f.command,f.rpRecordingGeneration)){
            nativeTransaction.stop(xess_vk::sr_replacement::Fault::IncompleteFrame);replacementFatalIfNeeded();}
    }
    void replacementReturned(const xess_vk::sr_observation::DispatchTicket& t) noexcept {
        if(!replacementAvailable||srSdkDepth)return;std::lock_guard lock(mutex);
        const auto now=GetTickCount64();
        if(!replacementSummaryAt||now-replacementSummaryAt>=60000){replacementSummaryAt=now;replacementReport("periodic");}
    }
    static void replacementArchiveLine(const char* text) noexcept {
        if(!text)return;const auto size=strnlen(text,32767);if(!size||size>=32767)return;
        char record[32768];std::memcpy(record,text,size);record[size]='\n';
        writeMeasurementSummaryLine(record,DWORD(size+1));
    }
    void replacementUse(xess_vk::descriptor_image_metadata::Key image,bool discard,const char* origin,
                        const xess_vk::CommandRecordingFacts& f={},xess_vk::descriptor_image_metadata::Key pipeline={},
                        xess_vk::descriptor_image_metadata::Key descriptorSet={}) noexcept {
        if(nativeTransaction.use(image,discard)){
            issue(replacementPlanFrame.engineFrame,origin);
            replacementFault.origin=origin;replacementFault.recording=f;replacementFault.image=image;replacementFault.pipeline=pipeline;
            replacementFault.contentOmitted=nativeTransaction.omitted(image);replacementFault.producerProfile=nativeTransaction.producer(image);
            replacementFault.missingContents=nativeTransaction.missingContents();replacementFault.frameOpen=nativeTransaction.open();
            replacementFault.frameReplacing=nativeTransaction.replacement();
            lastReplacementFault=replacementFault;lastReplacementFaultCode=nativeTransaction.fault();
            if(pipeline.handle&&replacementFaultPipelineCount<replacementFaultPipelines.size()&&
                std::find(replacementFaultPipelines.begin(),replacementFaultPipelines.end(),pipeline)==replacementFaultPipelines.end()){
                replacementFaultPipelines[replacementFaultPipelineCount++]=pipeline;
                xess_vk::compute_observer::selectArchivedShader(device->srShaders,
                    reinterpret_cast<VkPipeline>(pipeline.handle),pipeline.generation,replacementArchiveLine);
            }
            if(nativeTransaction.counts().guards>2&&!nativeTransaction.fatal())return;
            JsonLine line("sr_native_replacement_consumer");line.handle("image",image.handle);line.number("generation",image.generation);
            line.text("origin",origin);line.handle("command",f.command);line.number("recording",f.rpRecordingGeneration);
            line.number("order",f.commonRecordOrder);line.handle("pipeline",pipeline.handle);line.number("pipeline_generation",pipeline.generation);
            line.number("producer_profile",replacementFault.producerProfile);line.number("presents",presents);line.write();
            replacementReport("core_content_consumer");
        }
        replacementFatalIfNeeded();
    }
    void replacementImage(VkImage image,bool discard,const xess_vk::CommandRecordingFacts& f) noexcept {
        if(!replacementAvailable||srSdkDepth||!image)return;
        const auto found=device->imageInventory->lookup(bits(image));
        std::lock_guard lock(mutex);
        if(found.status==xess_vk::image_inventory::Status::LiveHandleObserved)replacementUse({found.record.image,found.record.generation},discard,"image_transfer_or_clear",f);
    }
    bool replacementFullClear(VkImage image,unsigned count,const VkImageSubresourceRange* ranges) noexcept {
        if(!replacementAvailable||count!=1||!ranges)return false;
        const auto found=device->imageInventory->lookup(bits(image));const auto& r=ranges[0];const auto& d=found.record.description;
        return found.status==xess_vk::image_inventory::Status::LiveHandleObserved&&xess_vk::sr_replacement::fullColorRange(r,d.mipLevels,d.arrayLayers);
    }
    template<class Barrier>
    void replacementUndefinedLayouts(const xess_vk::CommandRecordingFacts& f,unsigned count,const Barrier* barriers) noexcept {
        if(!replacementAvailable||srSdkDepth||!barriers)return;
        std::lock_guard lock(mutex);
        if(!f.scopeKnown||!f.command||!f.rpRecordingGeneration||f.device!=bits(identity.device)||f.contextIncarnation!=identity.incarnation||
           f.queueFamily!=device->srQueueFamily||f.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY||f.inRenderPass||f.inDynamicRendering||
           (ownerThread&&GetCurrentThreadId()!=ownerThread))return;
        if(!nativeTransaction.hasContents())return;
        // Scan the legal call's entire array, independently of trace quotas.
        // UNDEFINED->UNDEFINED is not a discard with synchronization2.
        for(unsigned n=0;n<count;++n){const auto& b=barriers[n];
            constexpr auto type=std::is_same_v<Barrier,VkImageMemoryBarrier>?
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER:VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            if(b.sType!=type||b.pNext||b.oldLayout!=VK_IMAGE_LAYOUT_UNDEFINED||b.newLayout==VK_IMAGE_LAYOUT_UNDEFINED||
               b.newLayout==VK_IMAGE_LAYOUT_PREINITIALIZED||b.newLayout==VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT||
               b.srcQueueFamilyIndex!=b.dstQueueFamilyIndex||!b.image)continue;
            const auto found=device->imageInventory->lookup(bits(b.image));const auto& d=found.record.description;
            const xess_vk::descriptor_image_metadata::Key image{found.record.image,found.record.generation};
            if(found.status!=xess_vk::image_inventory::Status::LiveHandleObserved||!nativeTransaction.watches(image)||
               d.depth!=1||!xess_vk::sr_replacement::fullColorRange(b.subresourceRange,d.mipLevels,d.arrayLayers))continue;
            const bool omitted=nativeTransaction.omitted(image);
            replacementUse(image,true,"undefined_layout_discard",f);
            if(omitted&&++replacementUndefinedLayoutDiscards<=3){JsonLine line("sr_undefined_layout_discard");
                line.handle("image",image.handle);line.number("image_generation",image.generation);line.boolean("old_content_omitted",omitted);
                line.handle("command",f.command);line.number("recording",f.rpRecordingGeneration);line.number("order",f.commonRecordOrder);
                line.number("new_layout",b.newLayout);line.writeObservation();}
        }
    }
    void replacementImageRetired(VkImage image) noexcept {
        if(!replacementAvailable||srSdkDepth)return;
        const auto s=device->imageInventory->lookup(bits(image));std::lock_guard lock(mutex);
        if(s.status==xess_vk::image_inventory::Status::LiveHandleObserved)nativeTransaction.retire({s.record.image,s.record.generation});
    }
    void replacementAttachment(VkImageView handle,VkAttachmentLoadOp load,VkRect2D area,std::uint32_t layers,bool known,
                               const xess_vk::CommandRecordingFacts& f) noexcept {
        const auto v=xess_vk::render_pass_observer::diagnosticViewSnapshot(device->renderPass,handle);
        if(!v.known||!v.image.handle)return;
        const auto found=device->imageInventory->lookup(v.image.handle);
        const auto& d=found.record.description;
        const bool discard=known&&!v.issues&&!v.hasPNext&&found.status==xess_vk::image_inventory::Status::LiveHandleObserved&&
            found.record.generation==v.image.generation&&(load==VK_ATTACHMENT_LOAD_OP_CLEAR||load==VK_ATTACHMENT_LOAD_OP_DONT_CARE)&&
            !area.offset.x&&!area.offset.y&&area.extent.width==d.width&&area.extent.height==d.height&&layers==1&&
            d.arrayLayers==1&&d.mipLevels==1&&xess_vk::sr_replacement::fullColorRange(v.rawRange,d.mipLevels,d.arrayLayers);
        if(!discard&&!nativeTransaction.stopped()&&nativeTransaction.watches({v.image.handle,v.image.generation})){
            replacementFault.issues=v.issues;replacementFault.area=area;replacementFault.range=v.rawRange;replacementFault.load=load;
            replacementFault.width=d.width;replacementFault.height=d.height;replacementFault.layers=layers;replacementFault.attachmentKnown=known;
        }
        if(!nativeTransaction.stopped()&&nativeTransaction.watches({v.image.handle,v.image.generation})&&
            ((!discard&&nativeTransaction.counts().guards<2)||nativeTransaction.counts().discards<8)){
            JsonLine line("sr_native_replacement_attachment");line.handle("image",v.image.handle);line.number("generation",v.image.generation);
            line.handle("command",f.command);line.number("recording",f.rpRecordingGeneration);line.number("order",f.commonRecordOrder);
            line.number("load_op",load);line.boolean("attachment_known",known);line.number("view_issues",v.issues);line.boolean("view_pnext",v.hasPNext);
            line.number("format",d.format);line.number("image_width",d.width);line.number("image_height",d.height);
            line.number("area_x",area.offset.x);line.number("area_y",area.offset.y);line.number("area_width",area.extent.width);line.number("area_height",area.extent.height);
            line.number("layers",layers);line.number("base_mip",v.rawRange.baseMipLevel);line.number("mip_count",v.rawRange.levelCount);
            line.number("base_layer",v.rawRange.baseArrayLayer);line.number("layer_count",v.rawRange.layerCount);line.boolean("full_discard",discard);line.writeObservation();
        }
        replacementUse({v.image.handle,v.image.generation},discard,"attachment",f);
    }
    template<class Command>
    bool replacementCommand(const xess_vk::CommandRecordingFacts& f,const Command& c) noexcept {
        if(!replacementAvailable||srSdkDepth)return false;namespace stc=xess_vk::sr_tail_commands;
        const bool draw=c.kind>=stc::Kind::Draw&&c.kind<=stc::Kind::DrawIndexedIndirectCount;
        const auto& s=c.descriptors;
        constexpr VkShaderStageFlags graphicsStages=VK_SHADER_STAGE_ALL_GRAPHICS|VK_SHADER_STAGE_TASK_BIT_EXT|VK_SHADER_STAGE_MESH_BIT_EXT;
        const bool stagesGraphics=s.bindPoint==VK_PIPELINE_BIND_POINT_MAX_ENUM&&(s.stages&graphicsStages);
        const bool graphicsBind=c.kind==stc::Kind::BindDescriptorSets&&(s.bindPoint==VK_PIPELINE_BIND_POINT_GRAPHICS||stagesGraphics);
        // Compute bindings are tracked by the compute observer, not this graphics guard.
        if(!draw&&!graphicsBind&&c.kind!=stc::Kind::BeginRenderPass&&c.kind!=stc::Kind::BeginRendering&&
           !(c.kind==stc::Kind::Other&&std::string_view(c.otherName.data())=="vkCmdExecuteCommands"))return false;
        const bool measured=trackingCpuTimingEnabled();const auto entered=trackingCpuStart();
        std::unique_lock lock(mutex,std::try_to_lock);if(!lock.owns_lock()){lock.lock();++replacementLockContended;}
        // The engine's own frames are forwarded untouched; a recording that began
        // here is re-captured at its next bind after the return.
        if(fallbackTransparent())return false;
        ++replacementCommandCalls;
        struct Time {xess_vk::CpuWallTime& total;xess_vk::CpuWallClock::time_point start;bool enabled;
            ~Time(){if(enabled)total.add(trackingCpuElapsedNs(start));}} time{replacementCommandCpu,entered,measured};
        // A live command owns at most one record (a new one is only created when
        // none matches), so the hinted record and the scan name the same entry.
        GraphicsBindings* g=nullptr;auto& hint=replacementGraphicsHints[graphicsHintIndex(f.command)];
        if(f.command&&hint&&hint<=replacementGraphics.size()&&replacementGraphics[hint-1].command==f.command)g=&replacementGraphics[hint-1];
        if(!g){++replacementGraphicsScans;
            for(auto& entry:replacementGraphics)if(entry.command==f.command){g=&entry;break;}
            if(!g)for(auto& entry:replacementGraphics)if(!entry.command){g=&entry;break;}
            if(!g){try{replacementGraphics.emplace_back();g=&replacementGraphics.back();}
                catch(...){replacementStop(xess_vk::sr_replacement::Fault::Capacity,"graphics_bindings_allocation_failed",f);replacementFatalIfNeeded();return false;}}
            if(f.command)hint=static_cast<std::uint32_t>(g-replacementGraphics.data())+1;
        }
        if(g->command!=f.command||g->recording!=f.rpRecordingGeneration)*g={f.command,f.rpRecordingGeneration,f.pool};
        namespace dm=xess_vk::descriptor_image_metadata;bool commandUnsafe=false;
        if(graphicsBind){
            ++replacementBindChecks;
            // Buffer dynamic-offset truncation cannot hide an image identity.
            const bool captured=!c.missingData&&s.firstSet<=g->sets.size()&&s.setCount<=g->sets.size()-s.firstSet&&s.storedSets==s.setCount;
            if(!captured)g->bindIssues|=dm::bit(dm::Issue::Capacity);
            if(c.extensionsOmitted||(stagesGraphics&&(s.stages&graphicsStages)!=VK_SHADER_STAGE_ALL_GRAPHICS))
                g->bindIssues|=dm::bit(dm::Issue::UnsupportedExtension);
            if(captured){
                bool same=true;
                for(unsigned n=0;n<s.storedSets;++n){const auto index=s.firstSet+n;const auto* token=g->masks[index].published;
                    same&=g->sets[index].handle==s.sets[n]&&token&&token->read().set==g->sets[index];}
                if(!same){
                    std::array<VkDescriptorSet,8> handles{};std::array<dm::ReferenceBinding,8> bindings{};
                    std::array<dm::ReferenceBinding,8> queried{};std::array<unsigned,8> indices{};unsigned misses{};
                    const auto epoch=xess_vk::descriptor_image_observer::referenceMaskEpoch(device->descriptors);
                    for(unsigned n=0;n<s.storedSets;++n)if(s.sets[n]){
                        bindings[n]=replacementIdentityCache.find(s.sets[n],epoch);
                        if(bindings[n].published)++replacementIdentityCacheHits;
                        else{++replacementIdentityCacheMisses;indices[misses]=n;handles[misses++]=reinterpret_cast<VkDescriptorSet>(s.sets[n]);}
                    }
                    if(misses){
                        xess_vk::descriptor_image_observer::referenceBindings(device->descriptors,{handles.data(),misses},{queried.data(),misses});
                        ++replacementIdentityBatches;replacementIdentityQueries+=misses;
                        for(unsigned n=0;n<misses;++n){bindings[indices[n]]=queried[n];replacementIdentityCache.remember(queried[n]);}
                    }
                    for(unsigned n=0;n<s.storedSets;++n){const dm::Key set{s.sets[n],bindings[n].set.generation};
                        g->sets[s.firstSet+n]=set;g->masks[s.firstSet+n]=bindings[n];
                        if(!s.sets[n])g->bindIssues|=dm::bit(dm::Issue::InvalidInput);
                    }
                }
            }
        }else if(draw){
            ++replacementDrawChecks;
            if(!nativeTransaction.hasContents())return false;
            const auto contents=nativeTransaction.contentsRevision();
            if(!contents||contents!=replacementMaskRevision){
                std::array<dm::Key,dm::referenceMaskBits> images{};std::size_t count{};
                replacementMasksAvailable=nativeTransaction.referenceMasks(images,count,replacementMissingMask);
                if(replacementMasksAvailable)replacementMasksAvailable=xess_vk::descriptor_image_observer::watchReferenceImages(device->descriptors,{images.data(),count})!=0;
                replacementMaskRevision=contents;++replacementMaskConfigurations;
            }
            auto epoch=xess_vk::descriptor_image_observer::referenceMaskEpoch(device->descriptors);
            bool refresh=false;
            for(unsigned n=0;n<g->sets.size();++n)if(g->sets[n].handle){const auto* token=g->masks[n].published;const auto value=token?token->read():dm::ReferenceMaskSnapshot{};
                refresh|=!value.known||value.set!=g->sets[n]||value.epoch!=epoch;}
            if(replacementMasksAvailable&&epoch&&refresh){
                std::array<VkDescriptorSet,8> handles{};for(unsigned n=0;n<handles.size();++n)handles[n]=reinterpret_cast<VkDescriptorSet>(g->sets[n].handle);
                xess_vk::descriptor_image_observer::referenceBindings(device->descriptors,handles,g->masks,xess_vk::descriptor_image_observer::ReferenceQuerySource::Draw);++replacementMaskRefreshes;
                epoch=xess_vk::descriptor_image_observer::referenceMaskEpoch(device->descriptors);
            }
            bool known=replacementMasksAvailable&&epoch&&!g->bindIssues&&!nativeTransaction.stopped();std::uint64_t referenced{};
            std::array<std::uint64_t,8> consumedPublications;
            for(unsigned n=0;n<g->sets.size();++n)if(g->sets[n].handle){const auto* token=g->masks[n].published;const auto value=token?token->read():dm::ReferenceMaskSnapshot{};
                if(measured)consumedPublications[n]=value.publication;
                known&=value.known&&value.set==g->sets[n]&&value.epoch==epoch;referenced|=value.mask;}
            if(known&&measured)for(unsigned n=0;n<g->sets.size();++n)if(g->sets[n].handle){dm::ReferenceMaskSnapshot value;value.known=true;value.publication=consumedPublications[n];g->masks[n].published->consume(value);}
            ++replacementMaskDraws;
            if(known&&!(referenced&replacementMissingMask)){++replacementSafeCacheHits;return false;}
            ++replacementMaskFallbacks;
            xess_vk::descriptor_image_observer::imageReferences(device->descriptors,g->sets,g->references);++replacementDescriptorBatches;
            for(const auto set:g->sets)replacementDescriptorQueries+=set.handle!=0;
            std::uint64_t issues=g->bindIssues;dm::Key failedSet{};unsigned failedIndex{};
            for(unsigned index=0;index<g->sets.size();++index)if(const auto set=g->sets[index];set.handle){
                const auto& snapshot=g->references[index];
                for(std::size_t n=0;n<snapshot.referencedCount;++n){++replacementOmittedLookups;commandUnsafe|=nativeTransaction.omitted(snapshot.referencedImages[n]);replacementUse(snapshot.referencedImages[n],false,"graphics_draw",f,{},snapshot.set);}
                const auto currentIssues=(!snapshot.imageReferencesKnown?(snapshot.issues|dm::bit(dm::Issue::UnknownObject)):0)|
                    (snapshot.set!=set?dm::bit(dm::Issue::StaleGeneration):0);
                // A bad old snapshot must not poison later, fully rebound draws.
                if(currentIssues&&!failedSet.handle){
                    failedSet=set;failedIndex=index;
                    if(!nativeTransaction.stopped()){
                        replacementFault.descriptorLayout=snapshot.layout;replacementFault.setIssues=snapshot.setIssues;
                        replacementFault.layoutIssues=snapshot.layoutIssues;replacementFault.declaredImages=snapshot.declaredImageCount;
                        replacementFault.layoutBindings=snapshot.layoutBindingCount;replacementFault.unwrittenImages=snapshot.unwrittenCount;
                    }
                }
                issues|=currentIssues;
            }
            commandUnsafe|=issues&&nativeTransaction.hasContents();
            if(issues&&nativeTransaction.hasContents()&&!nativeTransaction.stopped()){
                replacementFault.descriptorSet=failedSet;replacementFault.setIndex=failedIndex;replacementFault.bindIssues=g->bindIssues;
                replacementStop(xess_vk::sr_replacement::Fault::Consumer,"graphics_bindings_unknown",f,issues);
            }
        }else if(c.kind==stc::Kind::BeginRenderPass){
            VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=reinterpret_cast<VkRenderPass>(c.renderPass.renderPass);
            b.framebuffer=reinterpret_cast<VkFramebuffer>(c.renderPass.framebuffer);b.renderArea=c.renderPass.area;
            const auto s=xess_vk::render_pass_observer::diagnosticPassSnapshot(device->renderPass,reinterpret_cast<VkCommandBuffer>(f.command),&b);
            for(std::size_t n=0;n<s.count;++n)replacementAttachment(reinterpret_cast<VkImageView>(s.views[n].key.handle),s.pass.attachments[n].loadOp,
                b.renderArea,s.framebuffer.layers,s.pass.known&&!s.pass.issues&&!s.framebuffer.issues&&n<s.pass.copiedAttachmentCount&&!c.extensionsOmitted,f);
        }else if(c.kind==stc::Kind::BeginRendering){
            const auto& r=c.rendering;
            for(unsigned n=0;n<r.storedColors;++n)replacementAttachment(reinterpret_cast<VkImageView>(r.colors[n].view),r.colors[n].loadOp,r.area,r.layerCount,
                !c.truncated&&!c.extensionsOmitted&&!c.missingData&&!r.viewMask&&!r.flags,f);
            for(unsigned n=0;n<r.storedColors;++n)if(r.colors[n].resolveMode!=VK_RESOLVE_MODE_NONE)
                replacementAttachment(reinterpret_cast<VkImageView>(r.colors[n].resolveView),VK_ATTACHMENT_LOAD_OP_LOAD,r.area,r.layerCount,false,f);
            if(r.hasDepth)replacementAttachment(reinterpret_cast<VkImageView>(r.depth.view),r.depth.loadOp,r.area,r.layerCount,false,f);
            if(r.hasStencil)replacementAttachment(reinterpret_cast<VkImageView>(r.stencil.view),r.stencil.loadOp,r.area,r.layerCount,false,f);
        }else if(c.kind==stc::Kind::Other&&std::string_view(c.otherName.data())=="vkCmdExecuteCommands"&&nativeTransaction.hasContents())
            replacementStop(xess_vk::sr_replacement::Fault::Consumer,"execute_secondary_commands",f);
        replacementFatalIfNeeded();
        const bool suppress=draw&&commandUnsafe;if(suppress)++guardSuppressedCommands;
        return suppress;
    }
