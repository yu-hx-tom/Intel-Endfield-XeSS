// Private implementation included inside vulkan_observer.cpp's namespace.
namespace sro=xess_vk::sr_overlay;
struct GameSrRuntime {
    DeviceState* device;
    std::mutex mutex;
    sro::Overlay overlay;
    xess_vk::sr_game::RuntimeIdentity identity;
    sro::Configuration desired;
    bool desiredKnown{},stopped{},inputExtentRefused{};
    DWORD ownerThread{};
    std::uint64_t degradedFrame{},degradedQpc{},degradedNs{};
    std::atomic<std::uint64_t> rebuildRequests{};std::uint64_t recoveredRebuild{};bool recoveryTrackingLost{};
    std::uint64_t outputXess{},outputTransition{},outputUnavailable{},guardSuppressedCommands{};
    bool holdOutput{},replacementFaultHandled{},countingCoreRegion{};
    std::uint64_t classifiedFrame{};unsigned classifiedMode{};
    const char* outputMode{"initializing"};
    const char* activeReason{"initializing"};
    const char* lastIssueReason{"none"};
    const char* recoveryBlocked{"none"};
    std::uint64_t issueFrame{},successfulRecoveries{},lastRecoveryFrame{};
    std::uint64_t backendDiagnosticAt{};
    sro::Status backendLastRecord{sro::Status::Disabled};xess_result_t backendLastSdkResult{XESS_RESULT_SUCCESS};
    bool sdkRestartFailed{};unsigned sdkRecoveryAttempts{};std::uint64_t restartAttemptRebuild{UINT64_MAX};
    void issue(std::uint64_t frame,const char* reason) noexcept {
        activeReason=lastIssueReason=reason;issueFrame=frame;
    }
    void resumed(std::uint64_t frame) noexcept {
        if(std::string_view(activeReason)!="none"&&executed){++successfulRecoveries;lastRecoveryFrame=frame;}
        activeReason="none";recoveryBlocked="none";
        sdkRecoveryAttempts=0;sdkRestartFailed=false;
    }
    const char* reasonCategory()const noexcept {
        using F=xess_vk::sr_replacement::Fault;
        switch(nativeTransaction.fault()){
            case F::Consumer:return "consumer";case F::Capacity:return "tracking";
            case F::Sequence:case F::IncompleteFrame:
                return std::string_view(xess_vk::sr_status::category(activeReason))=="configuration"?"configuration":"recording";
            default:return xess_vk::sr_status::category(activeReason);
        }
    }
    void classify(std::uint64_t frame,unsigned mode) noexcept {
        if(!xess_vk::diagnostic_flags::runtimeDiagnostics())return;
        if(!frame||frame<classifiedFrame)return;
        std::uint64_t* counts[]{&outputXess,&outputTransition,&outputUnavailable};
        if(frame==classifiedFrame){if(classifiedMode==mode)return;--*counts[classifiedMode];}
        classifiedFrame=frame;classifiedMode=mode;++*counts[mode];
    }
    void degrade(std::uint64_t frame,const char* mode) noexcept {
        if(xess_vk::diagnostic_flags::runtimeDiagnostics()&&!degradedQpc){LARGE_INTEGER q{};QueryPerformanceCounter(&q);degradedQpc=q.QuadPart;degradedNs=xess_vk::cpuWallNowNs();degradedFrame=frame;}
        outputMode=mode;
        if(std::string_view(activeReason)=="none"||std::string_view(activeReason)=="initializing")issue(frame,mode);
    }
    // Fallback after a held stop: the engine is asked for its own native
    // rendering (on this build its Native-AA pass at scale 1, never FSR plus
    // XeSS). Once native frames are observed the guard stops intervening and
    // the game's own frames are presented; XeSS returns after the faulting
    // consumer has been quiet. Waiting keeps the original suppression and hold.
    enum class Fallback:unsigned {None,Waiting,Passthrough,Returning};
    Fallback fallback{Fallback::None};
    std::uint64_t presents{},fallbackAppliedBase{},fallbackSince{},fallbackUpscaleAt{},fallbackConsumerAt{};
    std::uint64_t fallbackQuietFrom{},fallbackReturnFrom{},fallbackLastReturn{},fallbackRecoveredAt{};
    bool fallbackNativeSeen{},fallbackRecovered{};unsigned fallbackLevel{};
    std::array<xess_vk::descriptor_image_metadata::Key,4> fallbackConsumers{};unsigned fallbackConsumerCount{};
    std::uint64_t fallbackEntries{},fallbackReleases{},fallbackReturns{},fallbackReturnTimeouts{},fallbackAbandoned{},fallbackForwarded{},fallbackUpscaleShown{};
    // Presents, not frames: a dispatch recorded after present P is shown at P+1.
    static constexpr std::uint64_t fallbackSettlePresents=2,fallbackQuietPresents=120,fallbackReturnPresents=30,fallbackRefaultPresents=600;
    // An engine that keeps one upscaling camera must not turn the fallback into a freeze.
    static constexpr std::uint64_t fallbackUpscaleTolerancePresents=60;
    static const char* fallbackName(Fallback f) noexcept {
        switch(f){case Fallback::Waiting:return "waiting_engine_native";case Fallback::Passthrough:return "engine_native";
            case Fallback::Returning:return "returning_to_xess";default:return "none";}
    }
    bool fallbackTransparent()const noexcept {return fallback==Fallback::Passthrough||fallback==Fallback::Returning;}
    std::uint64_t fallbackQuiet()const noexcept {return fallbackQuietPresents<<fallbackLevel;}
    void fallbackEvent(const char* event,const char* why="") noexcept {
        if(!diagnosticLoggingEnabled())return;
        const auto& native=xess_vk::sr_native_resolution::control;
        JsonLine line(event);line.text("reason",why);line.text("fallback",fallbackName(fallback));line.number("presents",presents);
        line.number("entries",fallbackEntries);line.number("level",fallbackLevel);line.number("quiet_presents",fallbackQuiet());
        line.text("engine_native_request",xess_vk::sr_native_resolution::name(native.reason.load()));
        line.number("engine_native_updates",native.applied.load());line.number("engine_native_updates_at_hold",fallbackAppliedBase);
        line.boolean("native_frame_seen",fallbackNativeSeen);line.boolean("output_held",holdOutput);
        line.number("consumer_pipelines",fallbackConsumerCount);line.handle("consumer_pipeline",fallbackConsumerCount?fallbackConsumers[0].handle:0);
        line.number("last_consumer_present",fallbackConsumerAt);line.number("last_upscale_present",fallbackUpscaleAt);
        line.number("missing_core_contents",nativeTransaction.missingContents());line.number("abandoned_contents",fallbackAbandoned);
        line.boolean("transaction_stopped",nativeTransaction.stopped());line.boolean("transaction_open",nativeTransaction.open());
        line.text("recovery_blocked",recoveryBlocked);line.number("forwarded_dispatches",fallbackForwarded);
        line.number("releases",fallbackReleases);line.number("returns",fallbackReturns);line.number("return_timeouts",fallbackReturnTimeouts);line.number("upscale_frames_shown",fallbackUpscaleShown);
        line.number("xess_executed",executed);line.write();
    }
    // Every output hold goes through here. Only the first stop names the consumer
    // whose silence permits a return; later Native-AA refusals are the fallback itself.
    void fallbackHold(const char* why,xess_vk::descriptor_image_metadata::Key consumer={}) noexcept {
        holdOutput=true;
        if(fallback!=Fallback::None)return;
        // Re-entering soon after a return means the quiet signal missed a consumer.
        fallbackLevel=fallbackLastReturn&&presents-fallbackLastReturn<fallbackRefaultPresents?std::min(fallbackLevel+1,5u):0;
        fallback=Fallback::Waiting;fallbackAppliedBase=xess_vk::sr_native_resolution::control.applied.load();
        fallbackSince=fallbackQuietFrom=presents;fallbackConsumerAt=fallbackUpscaleAt=0;fallbackNativeSeen=fallbackRecovered=false;
        fallbackConsumerCount=0;if(consumer.handle)fallbackConsumers[fallbackConsumerCount++]=consumer;
        ++fallbackEntries;fallbackEvent("sr_native_fallback_waiting",why);
    }
    void fallbackPresent() noexcept {
        switch(fallback){
        case Fallback::None:return;
        case Fallback::Waiting:
            if(!xess_vk::sr_native_resolution::control.requested())return;
            if(fallbackNativeSeen||xess_vk::sr_native_resolution::control.applied.load()>fallbackAppliedBase){
                fallback=Fallback::Passthrough;fallbackSince=fallbackQuietFrom=presents;holdOutput=true;fallbackEvent("sr_native_fallback_passthrough");}
            return;
        case Fallback::Passthrough:{
            // An upscaling Accumulate means the frame is not the engine's native image.
            const bool upscaled=presents<fallbackUpscaleAt+fallbackSettlePresents;
            const bool settled=presents>=fallbackSince+fallbackSettlePresents&&(!upscaled||presents>=fallbackSince+fallbackUpscaleTolerancePresents);
            if(holdOutput&&settled){holdOutput=false;++fallbackReleases;fallbackEvent("sr_native_fallback_presenting",upscaled?"upscale_tolerance":"native");}
            else if(!holdOutput&&!settled){holdOutput=true;fallbackEvent("sr_native_fallback_upscale_frame_held");}
            fallbackUpscaleShown+=!holdOutput&&upscaled;
            // Native frames stay visible until a boundary actually recovers the transaction.
            if(!holdOutput&&presents-std::max(fallbackQuietFrom,fallbackConsumerAt)>=fallbackQuiet()){
                fallback=Fallback::Returning;fallbackReturnFrom=presents;
                fallbackAbandoned+=nativeTransaction.abandonOmitted();fallbackEvent("sr_native_fallback_returning");
            }
            return;}
        case Fallback::Returning:
            if(fallbackRecovered&&fallbackUpscaleAt>fallbackRecoveredAt){
                fallback=Fallback::None;fallbackLastReturn=presents;++fallbackReturns;fallbackEvent("sr_native_fallback_returned");return;}
            if(presents-(fallbackRecovered?fallbackRecoveredAt:fallbackReturnFrom)<fallbackReturnPresents)return;
            ++fallbackReturnTimeouts;
            // Recovered but still native: the ordinary guard decides on the next frame.
            if(fallbackRecovered){fallback=Fallback::None;fallbackLastReturn=presents;fallbackEvent("sr_native_fallback_return_unconfirmed");}
            // Blocked (for example an SDK that cannot restart): keep the native frames, back off.
            else {fallback=Fallback::Passthrough;fallbackQuietFrom=presents;fallbackLevel=std::min(fallbackLevel+1,5u);
                fallbackEvent("sr_native_fallback_return_blocked");}
            return;
        }
    }
    bool holdingOutput() noexcept {
        std::lock_guard lock(mutex);
        const auto now=GetTickCount64();
        if(diagnosticLoggingEnabled()&&xess_vk::sr_backend::startup()==xess_vk::sr_backend::Kind::Fsr4XmxLite&&
           (!backendDiagnosticAt||now-backendDiagnosticAt>=2000)){
            backendDiagnosticAt=now;JsonLine line("sr_backend_runtime");line.number("schema",1);
            line.text("backend","fsr4_xmx_lite");line.text("model","4.1.2X Lite");line.boolean("context_ready",overlay.fsr4Bridge()&&overlay.state()==sro::State::Ready);
            line.text("record_result",sro::name(backendLastRecord));line.number("record_sdk_result",backendLastSdkResult);line.text("backend_error",overlay.lastBackendError());
            line.number("input_width",desired.input.width);line.number("input_height",desired.input.height);line.number("output_width",desired.output.width);line.number("output_height",desired.output.height);
            line.number("recorded_successes",executed);line.number("submitted_frames",overlay.fsr4Bridge()?overlay.fsr4Bridge()->submittedFrames():0);
            line.number("menu_mask_full_writes",replacementMenuMaskFullWrites);
            line.boolean("pending_recordings",overlay.pending());line.boolean("stopped",stopped||nativeTransaction.stopped());line.boolean("output_held",holdOutput);
            line.text("output_mode",outputMode);line.text("active_reason",activeReason);line.text("recovery_blocked",recoveryBlocked);line.text("fallback",fallbackName(fallback));line.write();
        }
        ++presents;fallbackPresent();
        // Engine field readback alone is not proof of a new, fully rendered GPU image.
        return holdOutput;
    }
    template<class Sink> void measurementFields(Sink& line) noexcept {
        std::lock_guard lock(mutex);line.number("sr_schema",4);line.text("sr_recording_scope","application_command_recorded_not_gpu_complete");line.boolean("sr_enabled",true);
        line.text("sr_backend",xess_vk::sr_backend::name(xess_vk::sr_backend::startup()));line.text("sr_model",xess_vk::sr_backend::startup()==xess_vk::sr_backend::Kind::Fsr4XmxLite?"4.1.2X Lite":"XeSS");
        line.number("sr_backend_submitted_frames",overlay.fsr4Bridge()?overlay.fsr4Bridge()->submittedFrames():0);
        line.text("sr_output_mode",outputMode);line.number("sr_recorded_frames",outputXess+outputTransition+outputUnavailable);
        line.number("sr_status_schema",1);line.text("sr_active_reason",activeReason);
        line.text("sr_state",holdOutput?"output_held":stopped||nativeTransaction.stopped()?"stopped":outputMode);
        line.text("sr_reason_category",reasonCategory());line.text("sr_last_issue_reason",lastIssueReason);
        line.number("sr_fault",unsigned(nativeTransaction.fault()));
        line.number("sr_last_issue_frame",issueFrame);line.number("sr_successful_recoveries",successfulRecoveries);line.number("sr_last_recovery_frame",lastRecoveryFrame);
        line.boolean("sr_currently_stopped",stopped||nativeTransaction.stopped());line.text("sr_recovery_blocked",recoveryBlocked);
        line.number("sr_input_width",desired.input.width);line.number("sr_input_height",desired.input.height);
        line.number("sr_output_width",desired.output.width);line.number("sr_output_height",desired.output.height);
        line.text("sr_engine_native_request",xess_vk::sr_native_resolution::name(xess_vk::sr_native_resolution::control.reason.load()));
        line.number("sr_guard_command_checks",replacementCommandCalls);line.number("sr_guard_draw_checks",replacementDrawChecks);
        line.number("sr_guard_bind_checks",replacementBindChecks);line.number("sr_guard_reference_queries",replacementDescriptorQueries);
        line.number("sr_guard_reference_batches",replacementDescriptorBatches);line.number("sr_guard_identity_queries",replacementIdentityQueries);
        line.number("sr_guard_identity_batches",replacementIdentityBatches);line.number("sr_guard_safe_cache_hits",replacementSafeCacheHits);
        line.number("sr_guard_explicit_omitted_lookups",replacementOmittedLookups);
        line.number("sr_guard_identity_cache_hits",replacementIdentityCacheHits);line.number("sr_guard_identity_cache_misses",replacementIdentityCacheMisses);
        line.number("sr_guard_schema",2);line.number("sr_guard_mask_draws",replacementMaskDraws);line.number("sr_guard_mask_fallbacks",replacementMaskFallbacks);
        line.number("sr_guard_mask_refreshes",replacementMaskRefreshes);line.number("sr_guard_mask_configurations",replacementMaskConfigurations);
        line.boolean("sr_tail_diagnostics_enabled",device->srTailDiagnostics);
        // Contention fields cover one entry each (replacementCommand; recordingFacts),
        // not every acquisition of either mutex.
        line.number("sr_hot_path_schema",1);line.number("sr_guard_runtime_lock_contended",replacementLockContended);
        line.number("sr_guard_binding_lookup_scans",replacementGraphicsScans);
        const auto facts=xess_vk::render_pass_observer::recordingFactsCounters(device->renderPass);
        line.number("rp_facts_queries",facts.queries);
        line.number("rp_facts_lock_contended",facts.lockContended);line.number("rp_command_lookup_scans",facts.lookupScans);
        line.number("sr_xess_recordings",overlay.fsr4Bridge()?0:outputXess);line.number("sr_fsr4_recordings",overlay.fsr4Bridge()?outputXess:0);line.number("sr_transition_recordings",outputTransition);line.number("sr_unavailable_recordings",outputUnavailable);
        line.number("sr_guard_suppressed_commands",guardSuppressedCommands);line.boolean("sr_output_held",holdOutput);
        line.text("sr_fallback_state",fallbackName(fallback));line.number("sr_fallback_entries",fallbackEntries);
        line.number("sr_fallback_releases",fallbackReleases);line.number("sr_fallback_returns",fallbackReturns);
        line.number("sr_fallback_return_timeouts",fallbackReturnTimeouts);line.number("sr_fallback_abandoned_contents",fallbackAbandoned);
        line.number("sr_fallback_forwarded_dispatches",fallbackForwarded);line.number("sr_fallback_upscale_frames_shown",fallbackUpscaleShown);
        line.number("sr_degraded_since_frame",degradedFrame);line.number("sr_degraded_since_qpc",degradedQpc);line.number("sr_degraded_since_ns",degradedNs);
        line.number("sr_engine_native_updates",xess_vk::sr_native_resolution::control.applied.load());
        line.number("sr_engine_native_recoveries",xess_vk::sr_native_resolution::control.recoveries.load());
        std::uint64_t forwarded{};for(auto c:nativeTransaction.counts().forwarded)forwarded+=c;
        line.number("sr_core_dispatch_forwarded",forwarded);line.number("sr_executes",executed);
        line.number("sr_submitted_batches",submittedBatches);line.number("sr_failed_submit_batches",failedSubmitBatches);
        line.boolean("sr_gpu_completion_display_join_verified",false);
        line.boolean("sr_native_resources_verified",false);
    }
    std::unique_ptr<xess_vk::sr_gpu_timestamps::Observer> timing;
    struct Span {xess_vk::CommandRecordingFacts recording; xess_vk::sr_gpu_timestamps::Identity identity; xess_vk::sr_gpu_timestamps::WriteTicket ticket;};
    std::array<Span,16> spans{};
    std::uint64_t timedFrames{},boundaryReports{},submittedBatches{},failedSubmitBatches{};
    bool timingIdle{};
    static void sample(void* user,const xess_vk::sr_gpu_timestamps::Sample& sample) noexcept {
        auto* self=static_cast<GameSrRuntime*>(user);const auto& i=sample.identity;
        if(sample.gpu.durationKnown){const unsigned group=i.pass==xess_vk::sr_gpu_timestamps::Pass::WholeFsr?(i.nativeCoreSuppressed?2:0):1;
            ++self->gpuSampleCount[group];self->gpuSampleNs[group]+=sample.gpu.durationNs;}
        JsonLine line("sr_gpu_timestamp_sample");line.handle("device",bits(self->device->device));
        line.text("pass",i.pass==xess_vk::sr_gpu_timestamps::Pass::WholeFsr?(i.nativeCoreSuppressed?"suppressed_fsr_region":"whole_fsr"):"xess");
        line.text("duration_scope",i.pass==xess_vk::sr_gpu_timestamps::Pass::WholeFsr?(i.nativeCoreSuppressed?"prepare_to_accumulate_without_native_core":"prepare_to_last_fsr"):"sdk_execute_and_input_output_barriers");
        line.number("engine_frame",i.frame.engineFrame);line.number("sdk_present_id",i.frame.ticket.sdkId);
        line.handle("command",sample.recording.command);line.number("recording_generation",sample.recording.rpRecordingGeneration);
        line.number("execution",sample.execution);line.boolean("whole_fsr_coverage_verified",i.pass==xess_vk::sr_gpu_timestamps::Pass::WholeFsr&&!i.nativeCoreSuppressed);
        gpuTimestampFields(line,"pass_gpu",sample.gpu);line.writeObservation();
    }
    void prepare(const xess_vk::CommandRecordingFacts& f,const xess_vk::sr_observation::DispatchTicket& t) noexcept {
        if(srSdkDepth||!t.containsPrepare||timedFrames>=16384||!device->srGpuRequested)return;
        std::lock_guard lock(mutex);
        if(!timing&&f.queueFamily<device->srGpuFamilyCount){
            const auto& family=device->srGpuFamilies[f.queueFamily];xess_vk::sr_gpu_timestamps::Config c;
            c.device=device->device;c.incarnation=f.contextIncarnation;c.window=t.window;c.queueFamily=f.queueFamily;
            c.validBits=family.timestampValidBits;c.queueFlags=family.queueFlags;c.periodNs=device->srGpuPeriod;
            c.functions=device->srGpuFunctions;c.sink=sample;c.sinkUser=this;c.enabled=true;
            timing=std::make_unique<xess_vk::sr_gpu_timestamps::Observer>(c);
        }
        if(!timing)return;
        auto& span=spans[t.frame.engineFrame%spans.size()];
        if(span.ticket)return;
        xess_vk::sr_gpu_timestamps::Identity i;i.window=t.window;i.ordinal=t.ordinal;i.pipeline=t.pipeline;
        i.pipelineGeneration=t.pipelineGeneration;i.frame=t.frame;i.groups=t.groups;i.render=t.renderExtent;i.pass=xess_vk::sr_gpu_timestamps::Pass::WholeFsr;
        const auto write=timing->beforeDispatch(f,i);if(write){span={f,i,write};++timedFrames;}
    }
    xess_vk::sr_gpu_timestamps::SubmitTicket beforeSubmit(const xess_vk::render_pass_observer::SubmitTicket& t) noexcept {
        if(srSdkDepth)return {};std::lock_guard lock(mutex);
        if(replacementAvailable&&nativeTransaction.replacement()){
            bool owns=t.issues!=0;
            for(std::size_t n=0;n<t.commandCount&&n<t.recordingFacts.size();++n)
                owns|=nativeTransaction.owns(t.recordingFacts[n].command,t.recordingFacts[n].rpRecordingGeneration);
            if(owns){nativeTransaction.stop(xess_vk::sr_replacement::Fault::IncompleteFrame);replacementFatalIfNeeded();}
        }
        if(!timing)return {};
        const auto count=std::min<std::size_t>(t.commandCount,t.recordingFacts.size());
        return timing->beforeSubmit(reinterpret_cast<VkQueue>(t.queue),t.queueFamily,{t.recordingFacts.data(),count},t.queueKnown&&!t.issues&&count==t.commandCount);
    }
    std::uint64_t executed{},attempts{},lastFrame{},sourceSkips{},intervalSkips{},bindingSkips{},guardFailures{},retainedContexts{},imageRefusals{};
    std::array<std::uint64_t,32> results{};
    std::uint32_t restoreHighWater{},restorePipelineSets{},restoreSets{};
    bool restoreKnown{},pushAbsent{};
    std::array<VkFormatFeatureFlags,4> features{};
    struct Guard {xess_vk::CommandRecordingFacts recording;bool pushSeen{},coreOmitted{};};
    std::vector<Guard> guards;
    const bool replacementAvailable{xess_vk::diagnostic_flags::srNativeReplacementAvailable()};
    xess_vk::sr_replacement::Transaction nativeTransaction;
    std::uint64_t replacementExecutedFrame{},replacementBlits{},replacementReports{},replacementSummaryAt{};
    std::uint64_t replacementExecuteCount{};unsigned consecutiveTransitions{};
    std::uint64_t applicationDispatches{},applicationForwarded{},applicationSuppressed{},unknownDispatches{};
    std::uint64_t replacementPhotoFullWrites{},replacementPhotoFullWritesSkipped{},replacementPhotoCoverageRefusals{};
    std::uint64_t replacementAuxFullWrites{},replacementAuxFullWritesSkipped{},replacementAuxCoverageRefusals{};
    std::uint64_t replacementMenuMaskFullWrites{},replacementMenuMaskSkipped{},replacementMenuMaskRefusals{};
    std::uint64_t fullWriteRetiredWhileSuppressed{};
    struct PendingFullWrite {xess_vk::CommandRecordingFacts recording; xess_vk::descriptor_image_metadata::Key image,pipeline;
        bool auxiliary{},full{},omitted{};std::uint32_t width{},height{};std::array<std::uint32_t,3> groups{};
        const char* refusal{"none"};std::uint32_t format{};int profile{};};
    std::vector<PendingFullWrite> pendingFullWrites;
    // Diagnostic mode only: first use of each (pipeline,set,binding) on a watched image.
    struct WatchedUse {xess_vk::descriptor_image_metadata::Key pipeline;std::uint32_t set{},binding{};};
    std::array<WatchedUse,96> replacementWatchedUses{};unsigned replacementWatchedUseCount{};
    std::uint64_t replacementPrepareOutputsHash{};unsigned replacementPrepareOutputReports{};
    std::uint64_t replacementNonReadingReferences{},replacementWatchedNonReadingReferences{};
    std::uint64_t replacementUndefinedLayoutDiscards{};
    xess_vk::CpuWallTime replacementCommandCpu;
    std::uint64_t replacementCommandCalls{};
    std::uint64_t replacementDrawChecks{},replacementBindChecks{},replacementDescriptorQueries{},replacementDescriptorBatches{},replacementIdentityQueries{},replacementIdentityBatches{},replacementSafeCacheHits{},replacementOmittedLookups{};
    std::uint64_t replacementMaskRevision{},replacementMissingMask{},replacementMaskDraws{},replacementMaskFallbacks{},replacementMaskRefreshes{},replacementMaskConfigurations{};
    bool replacementMasksAvailable{};
    std::uint64_t replacementPrepareReports{};
    struct ReplacementFault {
        const char* origin{"none"};xess_vk::CommandRecordingFacts recording;
        xess_vk::descriptor_image_metadata::Key image,pipeline;std::uint64_t issues{};
        xess_vk::descriptor_image_metadata::Key descriptorSet;
        std::uint32_t setIndex{};std::uint64_t bindIssues{};
        xess_vk::descriptor_image_metadata::Key descriptorLayout;
        std::uint64_t setIssues{},layoutIssues{},declaredImages{},layoutBindings{},unwrittenImages{};
        VkRect2D area{};VkImageSubresourceRange range{};VkAttachmentLoadOp load{};
        std::uint32_t width{},height{},layers{};bool attachmentKnown{};
        bool contentOmitted{},frameOpen{},frameReplacing{};unsigned missingContents{},producerProfile{};
        std::uint32_t binding{},arrayElement{};VkDescriptorType descriptorType{VK_DESCRIPTOR_TYPE_MAX_ENUM};
    } replacementFault,lastReplacementFault;
    xess_vk::sr_replacement::Fault lastReplacementFaultCode{};
    bool terminalSuccess{},currentReplacement{},replacementReset{};
    std::uint64_t nativeAaUnqualified{};
    const char* replacementTerminalReason{"not_entered"};
    const char* replacementFallbackRefusal{"not_attempted"};
    sro::Result replacementTerminalResult{sro::Status::FrameUnverified};
    xess_vk::FrameTimingAssociation replacementPlanFrame;
    std::array<std::uint64_t,3> gpuSampleCount{};
    std::array<double,3> gpuSampleNs{};
    struct PipelineProfile {xess_vk::descriptor_image_metadata::Key key;int profile{-1};xess_vk::spirv::ImageReads reads;bool characterMenu{};};
    std::array<PipelineProfile,256> replacementPipelines{};unsigned replacementPipelineSerial{};
    std::array<xess_vk::descriptor_image_metadata::Key,4> replacementFaultPipelines{};unsigned replacementFaultPipelineCount{};
    struct GraphicsBindings {std::uint64_t command{},recording{},pool{};
        std::array<xess_vk::descriptor_image_metadata::Key,8> sets{};std::uint64_t bindIssues{};
        std::array<xess_vk::descriptor_image_metadata::SetImageReferences,8> references{};
        std::array<xess_vk::descriptor_image_metadata::ReferenceBinding,8> masks{};};
    std::vector<GraphicsBindings> replacementGraphics;
    xess_vk::descriptor_image_metadata::ReferenceBindingCache replacementIdentityCache;
    std::uint64_t replacementIdentityCacheHits{},replacementIdentityCacheMisses{};
    std::array<std::uint32_t,256> replacementGraphicsHints{}; // record index+1 per command hash; revalidated on use
    std::uint64_t replacementGraphicsScans{},replacementLockContended{};
    static std::size_t graphicsHintIndex(std::uint64_t command) noexcept {return ((command>>4)^(command>>12)^(command>>20)^(command>>32))&255;}
    struct GraphicsCommand {
        inline static const xess_vk::sr_tail_commands::DescriptorSets emptySets{};
        inline static const xess_vk::sr_tail_commands::RenderPass emptyPass{};
        inline static const xess_vk::sr_tail_commands::Rendering emptyRendering{};
        xess_vk::sr_tail_commands::Kind kind;
        const xess_vk::sr_tail_commands::DescriptorSets& descriptors{emptySets};
        const xess_vk::sr_tail_commands::RenderPass& renderPass{emptyPass};
        const xess_vk::sr_tail_commands::Rendering& rendering{emptyRendering};
        std::string_view otherName{""};bool truncated{},missingData{},extensionsOmitted{};
    };
    bool bindGraphics(const xess_vk::CommandRecordingFacts& f,VkPipelineBindPoint point,VkPipelineLayout layout,
                      unsigned first,unsigned count,const VkDescriptorSet* sets,unsigned dynamicCount,const std::uint32_t* offsets) noexcept {
        namespace stc=xess_vk::sr_tail_commands;stc::DescriptorSets d;d.bindPoint=point;d.layout=bits(layout);d.firstSet=first;
        d.setCount=count;d.storedSets=sets?std::min<unsigned>(count,d.sets.size()):0;d.dynamicOffsetCount=dynamicCount;
        for(unsigned n=0;n<d.storedSets;++n)d.sets[n]=bits(sets[n]);
        GraphicsCommand c{stc::Kind::BindDescriptorSets,d};c.missingData=(count&&!sets)||(dynamicCount&&!offsets);
        return replacementCommand(f,c);
    }

#ifdef XESS_METADATA_CONSUMER_TEST
    GameSrRuntime(DeviceState* s,bool replacement):device(s),identity{VK_NULL_HANDLE,VK_NULL_HANDLE,s->device,s->deviceIncarnation},replacementAvailable(replacement){}
#endif
    explicit GameSrRuntime(DeviceState* s):device(s),identity{s->parent->instance,s->srSdkPhysical,s->device,s->deviceIncarnation} {
        if(!s->srSingleQueue||!s->parent->srCreation||!s->parent->srCreation->sdk||!s->srAdmission||!s->srAdmission->admitted())
            throw std::runtime_error("SR requires an admitted SDK and one graphics/compute queue");
        if(!s->parent->srCreation->sdk->pinUntilProcessExit())throw std::runtime_error("SR SDK module pin failed");
        const auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(s->parent->gipa(s->parent->instance,"vkGetPhysicalDeviceFormatProperties"));
        if(!properties)throw std::runtime_error("SR format query unavailable");
        const VkFormat formats[]{VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_D32_SFLOAT_S8_UINT,VK_FORMAT_R16G16B16A16_SFLOAT};
        for(unsigned n=0;n<4;++n){VkFormatProperties p{};properties(s->physical,formats[n],&p);features[n]=p.optimalTilingFeatures;}
    }
    static bool enter(void*,const sro::Owner&) noexcept {
        if(srSdkDepth)return false;
        ++srSdkDepth;++srTailInjectedDepth;return true;
    }
    static void leave(void*) noexcept {--srTailInjectedDepth;--srSdkDepth;}
    static sro::RecordingKey key(const xess_vk::CommandRecordingFacts& f) noexcept {
        return {f.device,f.contextIncarnation,f.command,f.rpRecordingGeneration};
    }
    bool profileReady() const noexcept {
#ifdef XESS_FSR_DETOUR_AVAILABLE
        return xess_vk::fsr_callback_install::currentContinuousReady(bits(device->device));
#else
        return false;
#endif
    }
    void report(const char* reason,sro::Result result={}) noexcept {
        // Explicit backend names keep new FSR4 records distinguishable from historical XeSS-only logs.
        JsonLine line("sr_runtime");line.number("schema",4);line.text("backend",xess_vk::sr_backend::name(xess_vk::sr_backend::startup()));line.text("backend_error",overlay.lastBackendError());line.text("recording_scope","application_command_recorded_not_gpu_complete");line.text("output_mode",outputMode);line.text("reason",reason);line.text("result",sro::name(result.status));
        line.number("sdk_result",result.sdkResult);line.text("sdk_operation",overlay.lastSdkOperation());line.number("attempts",attempts);line.number("executed",executed);
        line.number("calling_thread",GetCurrentThreadId());line.number("sdk_owner_thread",ownerThread);line.number("last_executed_frame",lastFrame);
        line.number("source_skips",sourceSkips);line.number("interval_skips",intervalSkips);line.number("binding_skips",bindingSkips);
        line.number("guard_failures",guardFailures);line.number("retained_contexts",retainedContexts);
        line.boolean("restore_known",restoreKnown);line.boolean("push_constants_absent",pushAbsent);line.number("restore_sets",restoreSets);
        line.number("restore_high_water",restoreHighWater);line.number("restore_pipeline_sets",restorePipelineSets);
        line.number("input_width",desired.input.width);line.number("input_height",desired.input.height);
        line.number("output_width",desired.output.width);line.number("output_height",desired.output.height);
        line.number("requested_quality",overlay.requestedQuality());line.number("effective_quality",overlay.effectiveQuality());
        line.boolean("jittered_mv",desired.jitteredMotion);line.boolean("pending_recordings",overlay.pending());
        line.boolean("stopped",stopped);line.boolean("native_fsr_core_permitted",false);line.boolean("stage1_fsr_retained",false);
        const auto& native=xess_vk::sr_native_resolution::control;
        line.text("engine_native_request",xess_vk::sr_native_resolution::name(native.reason.load()));line.number("engine_native_updates",native.applied.load());
        line.number("engine_native_target_width",std::uint32_t(native.targetExtent.load()));line.number("engine_native_target_height",std::uint32_t(native.targetExtent.load()>>32));
        line.boolean("engine_native_resources_verified",false);line.boolean("blocking_initial_build",true);
        line.key("result_counts");line.raw("[");for(unsigned n=0;n<results.size();++n){if(n)line.raw(",");char value[32];std::snprintf(value,sizeof(value),"%llu",static_cast<unsigned long long>(results[n]));line.raw(value);}line.raw("]");line.writeObservation();
    }
    void reject(const char* reason,std::uint64_t& counter) noexcept {
        ++counter;
        // A follow-up refusal is not a new root cause for an already stopped run.
        if((!stopped&&!nativeTransaction.stopped()&&!xess_vk::sr_native_resolution::control.requested())||
           std::string_view(activeReason)=="none"||std::string_view(activeReason)=="initializing")
            issue(replacementPlanFrame.engineFrame,reason);
        if(device->measurements){
            using C=xess_vk::measurement::Counter;device->measurements->increment(C::SrSkips);
            const std::string_view category=reasonCategory();
            device->measurements->increment(category=="sdk"?C::SrSkipSdk:category=="recording"?C::SrSkipRecording:
                category=="configuration"?C::SrSkipConfiguration:category=="input"?C::SrSkipInputs:C::SrSkipOther);
        }
        if(currentReplacement){replacementTerminalReason=reason;replacementTerminalResult={sro::Status::FrameUnverified};}
        if(currentReplacement||counter<=2||attempts%300==0)report(reason,{sro::Status::FrameUnverified});
    }
    void boundary() noexcept {
        if(srSdkDepth)return;
#ifdef XESS_FSR_DETOUR_AVAILABLE
        (void)xess_vk::fsr_callback_install::ensureNativeResolution(bits(device->device));
#endif
        std::lock_guard lock(mutex);
        struct UpdateBridge {DeviceState* device;sro::Overlay& overlay;~UpdateBridge(){publishFsr4Bridge(device,overlay.fsr4Bridge());}} updateBridge{device,overlay};
        // Old Unity CBs can remain allocated across settings changes even after
        // their entire SR continuation completed. They must not pin SR sizing.
        if(auto* bridge=overlay.fsr4Bridge())for(auto& g:guards)
            if(g.coreOmitted&&bridge->completed(key(g.recording))){
                g.coreOmitted=false;
                // Retire the recording owner only. Omitted image contents stay
                // watched until a proved overwrite/discard or native recovery.
                nativeTransaction.retired(g.recording.command,g.recording.rpRecordingGeneration);
            }
        overlay.pollBackendCompletions();
        const auto rebuild=rebuildRequests.load();
        const bool needsRecovery=stopped||nativeTransaction.stopped()||xess_vk::sr_native_resolution::control.requested();
        if(rebuild!=restartAttemptRebuild){sdkRestartFailed=false;sdkRecoveryAttempts=0;restartAttemptRebuild=rebuild;}
        // While the engine's own frames are presented, only the present-side
        // quiet check may start a return; a mid-frame recovery would flip modes.
        if(needsRecovery&&fallback!=Fallback::Passthrough){
            const xess_vk::sr_status::Recovery recovery{recoveryTrackingLost,overlay.pending(),
                std::any_of(guards.begin(),guards.end(),[](const Guard& g){return g.coreOmitted;}),
                std::any_of(pendingFullWrites.begin(),pendingFullWrites.end(),[](const PendingFullWrite& w){return w.recording.command;}),
                nativeTransaction.open(),nativeTransaction.missingContents()!=0,ownerThread&&GetCurrentThreadId()!=ownerThread,sdkRestartFailed};
            recoveryBlocked=recovery.blocked();
            if(std::string_view(recoveryBlocked)=="none"&&overlay.state()==sro::State::Failed){
                if(sdkRecoveryAttempts){sdkRestartFailed=true;recoveryBlocked="sdk_restart_failed";}
                else{++sdkRecoveryAttempts;SrSdkScope injected;const auto reset=overlay.stop(sro::HostPhase::OutsideApplicationRecording);
                    if(reset.status!=sro::Status::Disabled){sdkRestartFailed=true;recoveryBlocked="sdk_restart_failed";}}
            }
            if(std::string_view(recoveryBlocked)=="none"&&nativeTransaction.recover()){
                recoveredRebuild=rebuild;xess_vk::sr_native_resolution::control.recoverAfterRetirement();
                stopped=false;replacementFaultHandled=false;countingCoreRegion=false;replacementReset=true;consecutiveTransitions=0;
                outputMode="initializing";activeReason="recovery_pending_output";
                // Returning stays transparent until an upscaling frame proves the engine left native mode.
                if(fallback==Fallback::Returning){fallbackRecovered=true;fallbackRecoveredAt=presents;holdOutput=true;fallbackEvent("sr_native_fallback_recovered");}
                else fallback=Fallback::None;
                report("safe_retirement_recovery");
            }
        }
        if(stopped||!desiredKnown||GetCurrentThreadId()!=ownerThread)return;
        SrSdkScope injected;
        sro::Result result;
        const auto phase=sro::HostPhase::OutsideApplicationRecording;
        if(overlay.state()==sro::State::Disabled){
            sro::VulkanFunctions vk{device->cmdPipelineBarrier,device->cmdBindPipeline,device->cmdBindDescriptorSets,device->cmdPushConstants,enter,leave,this};
            vk.blit=device->cmdBlitImage;
            vk.gipa=device->parent->gipa;vk.gdpa=device->gdpa;vk.queueFamily=device->srQueueFamily;
            vk.physicalForInstanceDispatch=device->physical;
            // One blocking initial build avoids background compiler work racing
            // device destruction after Unity has joined its recording thread.
            result=overlay.start(sro::Enable::On,phase,identity,*device->srAdmission,desired,
                sro::functions(*device->parent->srCreation->sdk),vk,16,true);
        }
        if(overlay.state()==sro::State::Building)result=overlay.pollBuild(phase);
        if(overlay.state()==sro::State::NeedsInit||overlay.state()==sro::State::Ready)result=overlay.initialize(phase,desired);
        if(result.status==sro::Status::SdkFailure||result.status==sro::Status::InvalidConfiguration){stopped=true;issue(lastFrame,"sdk_initialization");xess_vk::sr_native_resolution::control.request(xess_vk::sr_native_resolution::Reason::SdkFailure);}
        const bool extentRefused=result.status==sro::Status::UnsupportedInputExtent;
        if(extentRefused){issue(lastFrame,"unsupported_extent");xess_vk::sr_native_resolution::control.request(xess_vk::sr_native_resolution::Reason::UnsupportedExtent);}
        const bool extentTransition=(extentRefused&&!inputExtentRefused)||(inputExtentRefused&&result.status==sro::Status::Ready);
        if(extentRefused||result.status==sro::Status::Ready)inputExtentRefused=extentRefused;
        if(boundaryReports++<8||extentTransition||result.status==sro::Status::SdkFailure||result.status==sro::Status::InvalidConfiguration)report("host_boundary",result);
    }
    Guard* guard(const xess_vk::CommandRecordingFacts& f) noexcept {
        if(!f.command||!f.rpRecordingGeneration)return nullptr;
        Guard* target=nullptr;
        for(auto& g:guards){
            if(g.recording.command==f.command&&g.recording.rpRecordingGeneration==f.rpRecordingGeneration)return &g;
            if(!g.recording.command&&!target)target=&g;
        }
        if(!target)try{guards.emplace_back();target=&guards.back();}catch(...){recoveryTrackingLost=true;return nullptr;}
        *target={f};return target;
    }
    void push(const xess_vk::CommandRecordingFacts& f,VkShaderStageFlags stages) noexcept {
        if(srSdkDepth||!(stages&VK_SHADER_STAGE_COMPUTE_BIT))return;
        std::lock_guard lock(mutex);const auto target=guard(f);
        if(!target){stopped=true;xess_vk::sr_native_resolution::control.request(xess_vk::sr_native_resolution::Reason::RuntimeUnavailable);report("push_tracking_allocation_failed");return;}
        target->pushSeen=true;
    }
    bool image(const xess_vk::descriptor_image_metadata::ImageBinding& b,unsigned index,const sro::Owner& owner,sro::Image& out) noexcept {
        const auto metadata=xess_vk::descriptor_image_observer::validateImageBinding(device->descriptors,b);
        const auto memory=xess_vk::resource_memory_observer::sourceQuery(device->resourceMemory);
        const auto m=memory.query?memory.query(memory.user,{b.image.handle,b.image.generation}):xess_vk::resource_memory_observer::Observation{};
        if(!metadata.accepted()||!m.queried||m.localIssues||m.globalIssues){
            ++imageRefusals;
            if(imageRefusals<=2){
                JsonLine line("sr_image_refused");line.number("role",index);line.handle("image",b.image.handle);line.number("generation",b.image.generation);
                line.number("metadata_issues",metadata.issues);line.boolean("memory_queried",m.queried);line.number("memory_local_issues",m.localIssues);line.number("memory_global_issues",m.globalIssues);
                line.boolean("memory_bounds_known",m.binding.bindingBoundsKnown);line.boolean("memory_alias_free",m.binding.observedAliasFree);line.number("overlap_count",m.binding.overlapCount);line.number("executed",executed);line.write();
                xess_vk::resource_memory_observer::summary(device->resourceMemory);
            }
            return false;
        }
        out.owner=owner;out.imageGeneration=b.image.generation;out.viewGeneration=b.view.generation;out.memory=m.binding;
        out.sdk.image=reinterpret_cast<VkImage>(b.image.handle);out.sdk.imageView=reinterpret_cast<VkImageView>(b.view.handle);
        out.sdk.format=b.viewFormat;out.sdk.width=b.imageDescription.width;out.sdk.height=b.imageDescription.height;out.sdk.subresourceRange=b.range;
        out.usage=b.imageDescription.usage;out.formatFeatures=features[index];out.currentLayout=b.declaredLayout;out.barrierRange=b.range;
        out.separateDepthStencilLayouts=device->srSeparateLayouts;
        if(index==2&&!out.separateDepthStencilLayouts)out.barrierRange.aspectMask|=VK_IMAGE_ASPECT_STENCIL_BIT;
        const auto& c=b.components;
        out.identityView=c.r==VK_COMPONENT_SWIZZLE_IDENTITY&&c.g==VK_COMPONENT_SWIZZLE_IDENTITY&&c.b==VK_COMPONENT_SWIZZLE_IDENTITY&&c.a==VK_COMPONENT_SWIZZLE_IDENTITY;
        out.ordinary2dSingleSample=b.viewType==VK_IMAGE_VIEW_TYPE_2D&&b.imageDescription.depth==1&&b.imageDescription.samples==VK_SAMPLE_COUNT_1_BIT&&
            !(b.imageDescription.flags&(VK_IMAGE_CREATE_SPARSE_BINDING_BIT|VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT|VK_IMAGE_CREATE_DISJOINT_BIT));
        out.stateAndOwnerVerified=true;
        return true;
    }
    void record(const xess_vk::CommandRecordingFacts& f,const xess_vk::descriptor_image_metadata::DispatchEvidence& evidence,
                const xess_vk::sr_observation::DispatchTicket& ticket) noexcept {
        if(srSdkDepth||!currentReplacement||!ticket.containsAccumulate)return;
        std::lock_guard lock(mutex);++attempts;
        terminalSuccess=false;
        restoreKnown=evidence.computeRestoreKnown;pushAbsent=evidence.computePushConstantsAbsent;restoreSets=evidence.computeRestoreSetCount;
        restoreHighWater=evidence.computeBoundSetHighWater;restorePipelineSets=evidence.computePipelineSetCount;
        const auto& source=ticket.source;
        if(xess_vk::sr_native_resolution::control.requested()||stopped||!profileReady()||!source.valid||!source.frame.valid||source.frame.engineFrame!=replacementPlanFrame.engineFrame||source.frame.ticket!=replacementPlanFrame.ticket||!nativeTransaction.owns(f.command,f.rpRecordingGeneration)||f.queueFamily!=device->srQueueFamily){reject("profile_or_frame_source",sourceSkips);return;}
        if(!xess_vk::compute_observer::archivedShaderMatches(device->srShaders,reinterpret_cast<VkPipeline>(source.preparePipeline.handle),source.preparePipeline.generation,
              "0be6b001c712d6094916e86a06e8aa77ad9d41158515e3c80b3060c9180d8575")||
           !xess_vk::compute_observer::archivedShaderMatches(device->srShaders,reinterpret_cast<VkPipeline>(source.accumulatePipeline.handle),source.accumulatePipeline.generation,
              "c5ae0cdc2ed88ed0f09cde26bf6390a124aae059e6687281bde21cb55318d09c")){reject("shader_identity",sourceSkips);return;}
        constexpr auto required=xess_vk::frame_input::Extents|xess_vk::frame_input::Jitter|xess_vk::frame_input::MotionScale|xess_vk::frame_input::Reset;
        const auto& constants=source.constants;
        if((constants.validityMask&required)!=required){reject("frame_constants",sourceSkips);return;}
        if(xess_vk::sr_backend::startup()==xess_vk::sr_backend::Kind::Fsr4XmxLite&&
           (!(constants.validityMask&xess_vk::frame_input::CameraPlanes)||
            !(std::isfinite(constants.cameraNear)&&std::isfinite(constants.cameraFar)&&constants.cameraNear>0&&constants.cameraFar>constants.cameraNear)||
            !(std::isfinite(constants.tanHalfFovHorizontal)&&constants.tanHalfFovHorizontal>0))){reject("fsr4_camera_constants",sourceSkips);return;}
        sro::Configuration config;
        config.input={std::uint32_t(constants.renderSize[0]),std::uint32_t(constants.renderSize[1])};
        config.output={std::uint32_t(constants.upscaleSize[0]),std::uint32_t(constants.upscaleSize[1])};
        config.domain=sro::ColorDomain::SceneLinearHdr;config.invertedDepth=true;
        config.velocityScaleX=constants.motionVectorScale[0]*config.input.width;
        config.velocityScaleY=constants.motionVectorScale[1]*config.input.height;
        config.jitteredMotion=xess_vk::upscale_tail_probe::motionVectorsJittered!=xess_vk::diagnostic_flags::srInvertJitteredMotion();
        if(!ownerThread)ownerThread=GetCurrentThreadId();
        desired=config;desiredKnown=true;
        const auto interval=xess_vk::render_pass_observer::imageIntervalQuery(device->renderPass);
        for(unsigned n=0;n<3;++n){const auto& read=n==0?source.accumulate:source.prepare;
            if(read.command!=f.command||read.contextIncarnation!=f.contextIncarnation||read.rpRecordingGeneration!=f.rpRecordingGeneration||read.commonRecordOrder>f.commonRecordOrder){reject("source_interval",intervalSkips);return;}
            if(read.commonRecordOrder==f.commonRecordOrder)continue;
            const auto result=interval.resolve(interval.user,read,f,{source.images[n].image.handle,source.images[n].image.generation});
            if(!result.queried||result.issues||!result.untouched){
                if(currentReplacement){JsonLine line("sr_replacement_interval_rejected");line.number("role",n);line.handle("image",source.images[n].image.handle);
                    line.number("frame",source.frame.engineFrame);line.boolean("queried",result.queried);line.number("issues",result.issues);line.boolean("untouched",result.untouched);
                    line.number("read_order",read.commonRecordOrder);line.number("use_order",f.commonRecordOrder);
                    line.number("last_explicit_overflow_order",result.lastExplicitOverflowOrder);line.number("last_touch_overflow_order",result.lastTouchOverflowOrder);
                    line.boolean("preceding_overflow_ignored",result.precedingOverflowIgnored);
                    line.number("barriers",result.barriers);line.number("attachments",result.attachments);line.number("transfers",result.transfers);line.number("clears",result.clears);line.writeObservation();}
                reject("source_interval",intervalSkips);return;}
        }
        if(!evidence.computeRestoreKnown||!evidence.computePushConstantsAbsent||evidence.computeRestoreSetCount!=2){reject("compute_restore_incomplete",bindingSkips);return;}
        for(const auto& g:guards)if(g.recording.command==f.command&&g.recording.rpRecordingGeneration==f.rpRecordingGeneration&&g.pushSeen){reject("compute_push_constants_seen",bindingSkips);return;}
        sro::Frame frame;frame.recording=f;frame.owner={f.device,f.contextIncarnation,f.command,f.rpRecordingGeneration,f.commonRecordOrder,source.frame.engineFrame};frame.configuration=config;
        auto output=source.images[3];
        for(std::size_t n=0;n<evidence.count;++n)if(evidence.bindings[n].image==output.image&&evidence.bindings[n].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)output=evidence.bindings[n];
        if(!image(source.images[0],0,frame.owner,frame.color)||!image(source.images[1],1,frame.owner,frame.motion)||
           !image(source.images[2],2,frame.owner,frame.depth)||!image(output,3,frame.owner,frame.nativeOutput)){reject("input_output_image_lifetime",bindingSkips);return;}
        auto& restore=frame.restore;restore.owner=frame.owner;restore.pipeline=reinterpret_cast<VkPipeline>(evidence.pipeline.handle);restore.pipelineGeneration=evidence.pipeline.generation;
        restore.layout=reinterpret_cast<VkPipelineLayout>(evidence.pipelineLayout.handle);restore.layoutGeneration=evidence.pipelineLayout.generation;
        restore.setCount=evidence.computeRestoreSetCount;restore.completeStaticBindings=restore.completePushConstants=true;
        for(unsigned n=0;n<restore.setCount;++n){restore.sets[n]=reinterpret_cast<VkDescriptorSet>(evidence.computeRestoreSets[n].handle);restore.setGenerations[n]=evidence.computeRestoreSets[n].generation;}
        frame.jitterX=constants.jitter[0];frame.jitterY=constants.jitter[1];frame.historyEpoch=identity.incarnation;
        if(std::isfinite(constants.deltaSeconds)&&constants.deltaSeconds>0)frame.frameTimeMs=std::clamp(constants.deltaSeconds*1000.0f,0.001f,1000.0f);
        if(constants.validityMask&xess_vk::frame_input::CameraPlanes){frame.cameraNear=constants.cameraNear;frame.cameraFar=constants.cameraFar;}
        if(std::isfinite(constants.tanHalfFovHorizontal)&&constants.tanHalfFovHorizontal>0)
            frame.cameraFov=2.0f*std::atan(constants.tanHalfFovHorizontal/(float(config.output.width)/config.output.height));
        frame.gameReset=constants.reset||!lastFrame||source.frame.engineFrame!=lastFrame+1||replacementReset;
        frame.pinnedModule=frame.sameFrameInputs=frame.domainAndDownstreamVerified=true;
        frame.finalFsrDispatchReturned=false;
        frame.nativeCoreReplaced=frame.wholeCoreTransactionVerified=frame.atAccumulateBoundary=currentReplacement;
        frame.preExposureConventionVerified=frame.motionConventionVerified=frame.cameraHistoryVerified=true;
        frame.applicationReferencesRetained=frame.aliasCoverageComplete=frame.sourcesUnchangedAtUse=true;
        xess_vk::sr_gpu_timestamps::WriteTicket sdkTime;
        if(timing){auto& span=spans[source.frame.engineFrame%spans.size()];
            if(span.ticket&&span.recording.command==f.command&&span.recording.rpRecordingGeneration==f.rpRecordingGeneration&&
               span.recording.commonRecordOrder==source.prepare.commonRecordOrder){
                auto terminal=span.identity;terminal.callback=ticket.callbacks[0];terminal.output=config.output;terminal.extentsKnown=true;
                terminal.nativeCoreSuppressed=currentReplacement;
                if(timing->finishSpan(span.ticket,terminal)&&overlay.state()==sro::State::Ready){
                    terminal.pass=xess_vk::sr_gpu_timestamps::Pass::XeSS;terminal.pipeline=ticket.pipeline;terminal.pipelineGeneration=ticket.pipelineGeneration;
                    terminal.ordinal=ticket.ordinal;terminal.groups=ticket.groups;sdkTime=timing->beforeDispatch(f,terminal);
                }
                span.ticket={};
            }
        }
        const auto result=overlay.record(frame);
        if(currentReplacement){replacementTerminalReason="overlay_record";replacementTerminalResult=result;}
        terminalSuccess=result.executeSucceeded;
        if(currentReplacement){
            replacementBlits+=result.fallbackRecorded;
            if(result.executeSucceeded||result.fallbackRecorded){replacementExecutedFrame=source.frame.engineFrame;replacementReset=false;nativeTransaction.use(source.images[3].image,true);}
            replacementExecuteCount+=result.executeSucceeded;
            if(result.fallbackRecorded){classify(source.frame.engineFrame,1);degrade(source.frame.engineFrame,"color_transition");stopped=true;xess_vk::sr_native_resolution::control.request(xess_vk::sr_native_resolution::Reason::SdkFailure);}
        }
        backendLastRecord=result.status;backendLastSdkResult=result.sdkResult;
        if(timing&&sdkTime)timing->afterDispatch(sdkTime,result.executeSucceeded||result.status==sro::Status::SdkFailureCommandsUncertain);const auto index=static_cast<unsigned>(result.status);if(index<results.size())++results[index];
        if(result.executeSucceeded){resumed(source.frame.engineFrame);holdOutput=false;++executed;if(device->measurements)device->measurements->increment(xess_vk::measurement::Counter::SrExecutes);classify(source.frame.engineFrame,0);outputMode=overlay.fsr4Bridge()?"fsr4_xmx_replacement":"xess_replacement";lastFrame=source.frame.engineFrame;replacementReset=false;consecutiveTransitions=0;}
        if(result.status==sro::Status::SdkFailure||result.status==sro::Status::SdkFailureCommandsUncertain){stopped=true;issue(source.frame.engineFrame,"sdk_execute");xess_vk::sr_native_resolution::control.request(xess_vk::sr_native_resolution::Reason::SdkFailure);}
        if(attempts<=8||attempts%300==0||(currentReplacement&&!result.executeSucceeded)||result.status==sro::Status::SdkFailureCommandsUncertain)report(ticket.containsRcas?"after_rcas":"after_accumulate",result);
    }
    void submit(const xess_vk::render_pass_observer::SubmitTicket& ticket,VkResult result,const xess_vk::sr_gpu_timestamps::SubmitTicket& timed) noexcept {
        if(srSdkDepth)return;std::lock_guard lock(mutex);
        if(timing){timing->submitted(timed,result);timingIdle=false;}
        bool relevant=false;
        const auto count=std::min<std::size_t>(ticket.commandCount,ticket.recordingFacts.size());
        for(std::size_t n=0;n<count;++n)relevant|=overlay.contains(key(ticket.recordingFacts[n]));
        if(!relevant)return;
        if(result!=VK_SUCCESS){
            ++failedSubmitBatches;fallbackHold("submission_failed");issue(lastFrame,"submission_failed");degrade(lastFrame,"submission_failed");
            stopped=true;xess_vk::sr_native_resolution::control.request(xess_vk::sr_native_resolution::Reason::RuntimeUnavailable);
        }else ++submittedBatches;
        if(ticket.issues||!ticket.queueKnown||ticket.queueFamily!=device->srQueueFamily||count!=ticket.commandCount){
            fallbackHold("submission_membership_unverified");issue(lastFrame,"submission_membership_unverified");degrade(lastFrame,"submission_membership_unverified");
            stopped=true;xess_vk::sr_native_resolution::control.request(xess_vk::sr_native_resolution::Reason::RuntimeUnavailable);report("submission_membership_unverified");return;
        }
        for(std::size_t n=0;n<count;++n)(void)overlay.submitted({key(ticket.recordingFacts[n]),reinterpret_cast<VkQueue>(ticket.queue),ticket.token,result});
    }
    void retire(const xess_vk::CommandRecordingFacts& f,sro::Discard why) noexcept {
        if(srSdkDepth)return;std::lock_guard lock(mutex);(void)overlay.retired(key(f),why);
        nativeTransaction.retired(f.command,f.rpRecordingGeneration);
        if(timing)timing->retire(f.command,f.rpRecordingGeneration);
        for(auto& span:spans)if(span.recording.command==f.command&&span.recording.rpRecordingGeneration==f.rpRecordingGeneration)span={};
        for(auto& g:guards)if(g.recording.command==f.command&&g.recording.rpRecordingGeneration==f.rpRecordingGeneration)g={};
        for(auto& w:pendingFullWrites)if(w.recording.command==f.command&&w.recording.rpRecordingGeneration==f.rpRecordingGeneration)w={};
        for(auto& g:replacementGraphics)if(g.command==f.command&&g.recording==f.rpRecordingGeneration)g={};
    }
    void pool(std::uint64_t handle,sro::Discard why) noexcept {
        if(srSdkDepth)return;std::lock_guard lock(mutex);overlay.poolRetired(handle,why);
        if(timing)timing->retirePool(handle);
        for(auto& span:spans)if(span.recording.pool==handle)span={};
        for(auto& g:guards)if(g.recording.pool==handle){nativeTransaction.retired(g.recording.command,g.recording.rpRecordingGeneration);g={};}
        for(auto& w:pendingFullWrites)if(w.recording.pool==handle)w={};
        for(auto& g:replacementGraphics)if(g.pool==handle)g={};
    }
    void idle(VkResult result) noexcept {if(srSdkDepth)return;std::lock_guard lock(mutex);(void)overlay.deviceIdle(identity,result);timingIdle=result==VK_SUCCESS;if(timing)timing->deviceIdle(result);}
    ~GameSrRuntime(){
        publishFsr4Bridge(device,nullptr);
        std::lock_guard lock(mutex);stopped=true;
        if(overlay.hasContext()&&!overlay.pending()&&GetCurrentThreadId()==ownerThread){SrSdkScope injected;(void)overlay.stop(sro::HostPhase::OutsideApplicationRecording);}
        if(overlay.hasContext())++retainedContexts;
        if(timing){timing->stop();if(timingIdle)(void)timing->releaseForDeviceDestruction(VK_SUCCESS);}
        report("device_destroy");
        if(replacementAvailable)replacementReport("device_destroy");
        // Wrong-thread or unretired contexts are retained with the pinned SDK;
        // native device destruction frees their Vulkan objects. No waits here.
    }
#include "sr_game_replacement.inc"
};
