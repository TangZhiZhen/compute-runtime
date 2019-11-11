/*
 * Copyright (C) 2018-2019 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "runtime/os_interface/linux/drm_command_stream.h"

using namespace OCLRT;

template <typename GfxFamily>
class TestedDrmCommandStreamReceiver : public DrmCommandStreamReceiver<GfxFamily> {
  public:
    using CommandStreamReceiver::commandStream;
    using CommandStreamReceiver::lastSentSliceCount;

    TestedDrmCommandStreamReceiver(Drm *drm, gemCloseWorkerMode mode, ExecutionEnvironment &executionEnvironment) : DrmCommandStreamReceiver<GfxFamily>(*platformDevices[0], drm, executionEnvironment, mode) {
    }
    TestedDrmCommandStreamReceiver(Drm *drm, ExecutionEnvironment &executionEnvironment) : DrmCommandStreamReceiver<GfxFamily>(*platformDevices[0], drm, executionEnvironment, gemCloseWorkerMode::gemCloseWorkerInactive) {
    }

    void overrideGemCloseWorkerOperationMode(gemCloseWorkerMode overrideValue) {
        this->gemCloseWorkerOperationMode = overrideValue;
    }

    void overrideDispatchPolicy(DispatchMode overrideValue) {
        this->dispatchMode = overrideValue;
    }

    bool isResident(BufferObject *bo) {
        bool resident = false;
        for (auto it : this->residency) {
            if (it == bo) {
                resident = true;
                break;
            }
        }
        return resident;
    }

    void makeNonResident(GraphicsAllocation &gfxAllocation) override {
        makeNonResidentResult.called = true;
        makeNonResidentResult.allocation = &gfxAllocation;
        DrmCommandStreamReceiver<GfxFamily>::makeNonResident(gfxAllocation);
    }

    const BufferObject *getResident(BufferObject *bo) {
        BufferObject *ret = nullptr;
        for (auto it : this->residency) {
            if (it == bo) {
                ret = it;
                break;
            }
        }
        return ret;
    }

    struct MakeResidentNonResidentResult {
        bool called;
        GraphicsAllocation *allocation;
    };

    MakeResidentNonResidentResult makeNonResidentResult;
    std::vector<BufferObject *> *getResidencyVector() { return &this->residency; }

    SubmissionAggregator *peekSubmissionAggregator() {
        return this->submissionAggregator.get();
    }
    void overrideSubmissionAggregator(SubmissionAggregator *newSubmissionsAggregator) {
        this->submissionAggregator.reset(newSubmissionsAggregator);
    }
    std::vector<drm_i915_gem_exec_object2> &getExecStorage() {
        return this->execObjectsStorage;
    }
};


