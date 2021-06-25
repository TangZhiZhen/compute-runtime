/*
 * Copyright (C) 2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_stream/command_stream_receiver.h"
#include "shared/source/command_stream/linear_stream.h"
#include "shared/source/command_stream/preemption.h"
#include "shared/source/command_stream/scratch_space_controller.h"
#include "shared/source/command_stream/scratch_space_controller_xehp_plus.h"
#include "shared/source/gmm_helper/gmm.h"
#include "shared/source/gmm_helper/gmm_helper.h"
#include "shared/source/helpers/blit_commands_helper.h"
#include "shared/source/helpers/hw_helper.h"
#include "shared/source/helpers/timestamp_packet.h"
#include "shared/source/os_interface/os_interface.h"
#include "shared/test/common/cmd_parse/hw_parse.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/common/helpers/engine_descriptor_helper.h"
#include "shared/test/common/helpers/unit_test_helper.h"
#include "shared/test/common/helpers/variable_backup.h"
#include "shared/test/common/mocks/ult_device_factory.h"
#include "shared/test/unit_test/utilities/base_object_utils.h"

#include "opencl/source/command_queue/command_queue_hw.h"
#include "opencl/source/command_queue/resource_barrier.h"
#include "opencl/source/mem_obj/buffer.h"
#include "opencl/test/unit_test/fixtures/cl_device_fixture.h"
#include "opencl/test/unit_test/mocks/mock_command_queue.h"
#include "opencl/test/unit_test/mocks/mock_context.h"
#include "opencl/test/unit_test/mocks/mock_csr.h"
#include "opencl/test/unit_test/mocks/mock_event.h"
#include "opencl/test/unit_test/mocks/mock_kernel.h"
#include "opencl/test/unit_test/mocks/mock_memory_manager.h"
#include "opencl/test/unit_test/mocks/mock_platform.h"
#include "opencl/test/unit_test/mocks/mock_scratch_space_controller_xehp_plus.h"
#include "opencl/test/unit_test/mocks/mock_timestamp_container.h"
#include "test.h"

#include "gtest/gtest.h"
#include "reg_configs_common.h"

using namespace NEO;

namespace NEO {
template <typename GfxFamily>
class ImplicitFlushSettings {
  public:
    static bool &getSettingForNewResource();
    static bool &getSettingForGpuIdle();

  private:
    static bool defaultSettingForNewResource;
    static bool defaultSettingForGpuIdle;
};
} // namespace NEO

struct CommandStreamReceiverHwTestXeHPPlus : public ClDeviceFixture,
                                             public HardwareParse,
                                             public ::testing::Test {

    void SetUp() override {
        ClDeviceFixture::SetUp();
        HardwareParse::SetUp();
    }

    void TearDown() override {
        HardwareParse::TearDown();
        ClDeviceFixture::TearDown();
    }
};

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenPreambleSentWhenL3ConfigRequestChangedThenDontProgramL3Register) {
    using MI_LOAD_REGISTER_IMM = typename FamilyType::MI_LOAD_REGISTER_IMM;

    size_t GWS = 1;
    MockContext ctx(pClDevice);
    MockKernelWithInternals kernel(*pClDevice);
    CommandQueueHw<FamilyType> commandQueue(&ctx, pClDevice, 0, false);
    auto commandStreamReceiver = new MockCsrHw<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());

    pDevice->resetCommandStreamReceiver(commandStreamReceiver);
    auto &commandStreamCSR = commandStreamReceiver->getCS();

    PreemptionMode initialPreemptionMode = commandStreamReceiver->lastPreemptionMode;
    PreemptionMode devicePreemptionMode = pDevice->getPreemptionMode();

    commandStreamReceiver->isPreambleSent = true;
    commandStreamReceiver->lastSentL3Config = 0;

    commandQueue.enqueueKernel(kernel, 1, nullptr, &GWS, nullptr, 0, nullptr, nullptr);

    parseCommands<FamilyType>(commandStreamCSR, 0);
    auto itorCmd = find<MI_LOAD_REGISTER_IMM *>(cmdList.begin(), cmdList.end());
    if (PreemptionHelper::getRequiredCmdStreamSize<FamilyType>(initialPreemptionMode, devicePreemptionMode) > 0u) {
        ASSERT_NE(cmdList.end(), itorCmd);
    } else {
        EXPECT_EQ(cmdList.end(), itorCmd);
    }
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, WhenCommandStreamReceiverHwIsCreatedThenDefaultSshSizeIs2MB) {
    auto &commandStreamReceiver = pDevice->getGpgpuCommandStreamReceiver();
    EXPECT_EQ(2 * MB, commandStreamReceiver.defaultSshSize);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, WhenScratchSpaceExistsThenReturnNonZeroGpuAddressToPatch) {
    auto commandStreamReceiver = new MockCsrHw<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(commandStreamReceiver);
    void *ssh = alignedMalloc(512, 4096);

    uint32_t perThreadScratchSize = 0x400;

    bool stateBaseAddressDirty = false;
    bool cfeStateDirty = false;
    commandStreamReceiver->getScratchSpaceController()->setRequiredScratchSpace(ssh, 0u, perThreadScratchSize, 0u, 0u, *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    ASSERT_NE(nullptr, commandStreamReceiver->getScratchAllocation());
    EXPECT_TRUE(cfeStateDirty);

    auto scratchSpaceAddr = commandStreamReceiver->getScratchPatchAddress();
    constexpr uint64_t notExpectedScratchGpuAddr = 0;
    EXPECT_NE(notExpectedScratchGpuAddr, scratchSpaceAddr);
    alignedFree(ssh);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, WhenOsContextSupportsMultipleDevicesThenScratchSpaceAllocationIsPlacedOnEachSupportedDevice) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.CreateMultipleSubDevices.set(2u);
    ExecutionEnvironment *executionEnvironment = platform()->peekExecutionEnvironment();
    executionEnvironment->memoryManager.reset(new MockMemoryManager(false, true, *executionEnvironment));
    uint32_t tileMask = 0b11;
    std::unique_ptr<OsContext> osContext(OsContext::create(nullptr, 0u, EngineDescriptorHelper::getDefaultDescriptor({aub_stream::ENGINE_CCS, EngineUsage::Regular}, PreemptionMode::MidThread, tileMask)));
    auto commandStreamReceiver = std::make_unique<MockCsrHw<FamilyType>>(*executionEnvironment, 0, tileMask);
    initPlatform();

    void *ssh = alignedMalloc(512, 4096);

    uint32_t perThreadScratchSize = 0x400;

    bool stateBaseAddressDirty = false;
    bool cfeStateDirty = false;
    commandStreamReceiver->getScratchSpaceController()->setRequiredScratchSpace(ssh, 0u, perThreadScratchSize, 0u, 0u, *osContext, stateBaseAddressDirty, cfeStateDirty);
    auto allocation = commandStreamReceiver->getScratchAllocation();
    EXPECT_EQ(tileMask, static_cast<uint32_t>(allocation->storageInfo.memoryBanks.to_ulong()));
    alignedFree(ssh);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, WhenScratchSpaceNotExistThenReturnZeroGpuAddressToPatch) {
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());

    auto scratchSpaceAddr = commandStreamReceiver.getScratchPatchAddress();
    constexpr uint64_t expectedScratchGpuAddr = 0;
    EXPECT_EQ(expectedScratchGpuAddr, scratchSpaceAddr);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, whenProgrammingMiSemaphoreWaitThenSetRegisterPollModeMemoryPoll) {
    using MI_SEMAPHORE_WAIT = typename FamilyType::MI_SEMAPHORE_WAIT;
    MI_SEMAPHORE_WAIT miSemaphoreWait = FamilyType::cmdInitMiSemaphoreWait;
    EXPECT_EQ(MI_SEMAPHORE_WAIT::REGISTER_POLL_MODE::REGISTER_POLL_MODE_MEMORY_POLL, miSemaphoreWait.getRegisterPollMode());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceSurfaceStateEnabledWhenSratchAllocationRequestedThenProgramCfeStateWithScratchAllocation) {
    using CFE_STATE = typename FamilyType::CFE_STATE;
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;

    const HardwareInfo &hwInfo = *defaultHwInfo;
    size_t GWS = 1;
    MockContext ctx(pClDevice);
    MockKernelWithInternals kernel(*pClDevice);
    CommandQueueHw<FamilyType> commandQueue(&ctx, pClDevice, 0, false);
    auto commandStreamReceiver = new MockCsrHw<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver->getScratchSpaceController());
    scratchController->slotId = 2u;
    pDevice->resetCommandStreamReceiver(commandStreamReceiver);
    auto &commandStreamCSR = commandStreamReceiver->getCS();

    kernel.kernelInfo.kernelDescriptor.kernelAttributes.perThreadScratchSize[0] = 0x1000;
    auto &hwHelper = HwHelper::get(hwInfo.platform.eRenderCoreFamily);
    uint32_t computeUnits = hwHelper.getComputeUnitsUsedForScratch(&hwInfo);
    size_t scratchSpaceSize = kernel.kernelInfo.kernelDescriptor.kernelAttributes.perThreadScratchSize[0] * computeUnits;

    commandQueue.enqueueKernel(kernel, 1, nullptr, &GWS, nullptr, 0, nullptr, nullptr);
    commandQueue.flush();

    parseCommands<FamilyType>(commandStreamCSR, 0);
    findHardwareCommands<FamilyType>();

    EXPECT_EQ(kernel.kernelInfo.kernelDescriptor.kernelAttributes.perThreadScratchSize[0], commandStreamReceiver->requiredScratchSize);
    EXPECT_EQ(scratchSpaceSize, scratchController->scratchSizeBytes);
    EXPECT_EQ(scratchSpaceSize, scratchController->getScratchSpaceAllocation()->getUnderlyingBufferSize());
    ASSERT_NE(nullptr, cmdMediaVfeState);
    auto cfeState = static_cast<CFE_STATE *>(cmdMediaVfeState);
    uint32_t bufferOffset = static_cast<uint32_t>(scratchController->slotId * scratchController->singleSurfaceStateSize * 2);
    EXPECT_EQ(bufferOffset, cfeState->getScratchSpaceBuffer());
    RENDER_SURFACE_STATE *scratchState = reinterpret_cast<RENDER_SURFACE_STATE *>(scratchController->surfaceStateHeap + bufferOffset);
    EXPECT_EQ(scratchController->scratchAllocation->getGpuAddress(), scratchState->getSurfaceBaseAddress());
    EXPECT_EQ(RENDER_SURFACE_STATE::SURFACE_TYPE_SURFTYPE_SCRATCH, scratchState->getSurfaceType());

    SURFACE_STATE_BUFFER_LENGTH length = {0};
    length.Length = static_cast<uint32_t>(computeUnits - 1);
    EXPECT_EQ(length.SurfaceState.Depth + 1u, scratchState->getDepth());
    EXPECT_EQ(length.SurfaceState.Width + 1u, scratchState->getWidth());
    EXPECT_EQ(length.SurfaceState.Height + 1u, scratchState->getHeight());
    EXPECT_EQ(kernel.kernelInfo.kernelDescriptor.kernelAttributes.perThreadScratchSize[0], scratchState->getSurfacePitch());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceSurfaceStateEnabledWhenNewSshProvidedAndNoScratchAllocationExistThenNoDirtyBitSet) {
    auto commandStreamReceiver = std::make_unique<MockCsrHw<FamilyType>>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver->getScratchSpaceController());

    bool stateBaseAddressDirty = false;
    bool cfeStateDirty = false;
    scratchController->surfaceStateHeap = reinterpret_cast<char *>(0x1000);
    scratchController->setRequiredScratchSpace(reinterpret_cast<void *>(0x2000), 0u, 0u, 0u, 0u, *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_EQ(scratchController->surfaceStateHeap, reinterpret_cast<char *>(0x2000));
    EXPECT_FALSE(cfeStateDirty);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceSurfaceStateEnabledWhenRequiredScratchSpaceIsSetThenPerThreadScratchSizeIsAlignedTo64) {
    auto commandStreamReceiver = std::make_unique<MockCsrHw<FamilyType>>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver->getScratchSpaceController());

    uint32_t perThreadScratchSize = 1;
    uint32_t expectedValue = 1 << 6;
    bool stateBaseAddressDirty = false;
    bool cfeStateDirty = false;
    uint8_t surfaceHeap[1000];
    scratchController->setRequiredScratchSpace(surfaceHeap, 0u, perThreadScratchSize, 0u, commandStreamReceiver->taskCount, *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_EQ(expectedValue, scratchController->perThreadScratchSize);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceSurfaceStateEnabledWhenNewSshProvidedAndScratchAllocationExistsThenSetDirtyBitCopyCurrentState) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    auto commandStreamReceiver = std::make_unique<MockCsrHw<FamilyType>>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver->getScratchSpaceController());
    scratchController->slotId = 0;
    bool stateBaseAddressDirty = false;
    bool cfeStateDirty = false;

    void *oldSurfaceHeap = alignedMalloc(0x1000, 0x1000);
    scratchController->setRequiredScratchSpace(oldSurfaceHeap, 0u, 0x1000u, 0u, commandStreamReceiver->taskCount, *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_TRUE(cfeStateDirty);
    EXPECT_EQ(1u, scratchController->slotId);
    EXPECT_EQ(scratchController->surfaceStateHeap, oldSurfaceHeap);
    char *surfaceStateBuf = static_cast<char *>(oldSurfaceHeap) + scratchController->slotId * sizeof(RENDER_SURFACE_STATE) * 2;
    GraphicsAllocation *scratchAllocation = scratchController->scratchAllocation;
    RENDER_SURFACE_STATE *surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(surfaceStateBuf);
    EXPECT_EQ(scratchController->scratchAllocation->getGpuAddress(), surfaceState->getSurfaceBaseAddress());
    EXPECT_EQ(RENDER_SURFACE_STATE::SURFACE_TYPE_SURFTYPE_SCRATCH, surfaceState->getSurfaceType());

    void *newSurfaceHeap = alignedMalloc(0x1000, 0x1000);
    scratchController->setRequiredScratchSpace(newSurfaceHeap, 0u, 0x1000u, 0u, commandStreamReceiver->taskCount, *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_TRUE(cfeStateDirty);
    EXPECT_EQ(1u, scratchController->slotId);
    EXPECT_EQ(scratchController->surfaceStateHeap, newSurfaceHeap);
    EXPECT_EQ(scratchAllocation, scratchController->scratchAllocation);
    surfaceStateBuf = static_cast<char *>(newSurfaceHeap) + scratchController->slotId * sizeof(RENDER_SURFACE_STATE) * 2;
    surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(surfaceStateBuf);
    EXPECT_EQ(scratchController->scratchAllocation->getGpuAddress(), surfaceState->getSurfaceBaseAddress());
    EXPECT_EQ(RENDER_SURFACE_STATE::SURFACE_TYPE_SURFTYPE_SCRATCH, surfaceState->getSurfaceType());

    alignedFree(oldSurfaceHeap);
    alignedFree(newSurfaceHeap);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceSurfaceStateEnabledWhenBiggerScratchSpaceRequiredThenReplaceAllocation) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    auto commandStreamReceiver = new MockCsrHw<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver->getScratchSpaceController());
    scratchController->slotId = 6;

    pDevice->resetCommandStreamReceiver(commandStreamReceiver);

    bool cfeStateDirty = false;
    bool stateBaseAddressDirty = false;

    void *surfaceHeap = alignedMalloc(0x1000, 0x1000);
    scratchController->setRequiredScratchSpace(surfaceHeap, 0u, 0x1000u, 0u, commandStreamReceiver->taskCount,
                                               *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_TRUE(cfeStateDirty);
    EXPECT_EQ(7u, scratchController->slotId);
    uint64_t offset = static_cast<uint64_t>(scratchController->slotId * sizeof(RENDER_SURFACE_STATE) * 2);
    EXPECT_EQ(offset, scratchController->getScratchPatchAddress());
    EXPECT_EQ(0u, scratchController->calculateNewGSH());
    uint64_t gpuVa = scratchController->scratchAllocation->getGpuAddress();
    char *surfaceStateBuf = static_cast<char *>(scratchController->surfaceStateHeap) + offset;
    RENDER_SURFACE_STATE *surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(surfaceStateBuf);
    EXPECT_EQ(gpuVa, surfaceState->getSurfaceBaseAddress());

    scratchController->setRequiredScratchSpace(surfaceHeap, 0u, 0x2000u, 0u, commandStreamReceiver->taskCount,
                                               *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_TRUE(cfeStateDirty);
    EXPECT_EQ(8u, scratchController->slotId);
    offset = static_cast<uint64_t>(scratchController->slotId * sizeof(RENDER_SURFACE_STATE) * 2);
    EXPECT_EQ(offset, scratchController->getScratchPatchAddress());
    EXPECT_NE(gpuVa, scratchController->scratchAllocation->getGpuAddress());
    gpuVa = scratchController->scratchAllocation->getGpuAddress();
    surfaceStateBuf = static_cast<char *>(scratchController->surfaceStateHeap) + offset;
    surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(surfaceStateBuf);
    EXPECT_EQ(gpuVa, surfaceState->getSurfaceBaseAddress());

    alignedFree(surfaceHeap);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceSurfaceStateEnabledWhenScratchSlotIsNonZeroThenSlotIdIsUpdatedAndCorrectOffsetIsSet) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    auto commandStreamReceiver = new MockCsrHw<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver->getScratchSpaceController());

    pDevice->resetCommandStreamReceiver(commandStreamReceiver);

    bool cfeStateDirty = false;
    bool stateBaseAddressDirty = false;

    void *surfaceHeap = alignedMalloc(0x1000, 0x1000);
    scratchController->setRequiredScratchSpace(surfaceHeap, 1u, 0x1000u, 0u, commandStreamReceiver->taskCount,
                                               *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_TRUE(cfeStateDirty);
    EXPECT_EQ(1u, scratchController->slotId);
    EXPECT_TRUE(scratchController->updateSlots);
    uint64_t offset = static_cast<uint64_t>(scratchController->slotId * sizeof(RENDER_SURFACE_STATE) * 2);
    EXPECT_EQ(offset, scratchController->getScratchPatchAddress());
    EXPECT_EQ(0u, scratchController->calculateNewGSH());
    uint64_t gpuVa = scratchController->scratchAllocation->getGpuAddress();
    char *surfaceStateBuf = static_cast<char *>(scratchController->surfaceStateHeap) + offset;
    RENDER_SURFACE_STATE *surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(surfaceStateBuf);
    EXPECT_EQ(gpuVa, surfaceState->getSurfaceBaseAddress());
    alignedFree(surfaceHeap);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceSurfaceStateEnabledWhenProgramHeapsThenSetReqScratchSpaceAndProgramSurfaceStateAreCalled) {
    class MockScratchSpaceControllerXeHPPlus : public ScratchSpaceControllerXeHPPlus {
      public:
        uint32_t requiredScratchSpaceCalledTimes = 0u;
        uint32_t programSurfaceStateCalledTimes = 0u;
        MockScratchSpaceControllerXeHPPlus(uint32_t rootDeviceIndex,
                                           ExecutionEnvironment &environment,
                                           InternalAllocationStorage &allocationStorage) : ScratchSpaceControllerXeHPPlus(rootDeviceIndex, environment, allocationStorage) {}

        using ScratchSpaceControllerXeHPPlus::scratchAllocation;

        void setRequiredScratchSpace(void *sshBaseAddress,
                                     uint32_t scratchSlot,
                                     uint32_t requiredPerThreadScratchSize,
                                     uint32_t requiredPerThreadPrivateScratchSize,
                                     uint32_t currentTaskCount,
                                     OsContext &osContext,
                                     bool &stateBaseAddressDirty,
                                     bool &vfeStateDirty) override {
            requiredScratchSpaceCalledTimes++;
        }

      protected:
        void programSurfaceState() override {
            programSurfaceStateCalledTimes++;
        };
    };

    auto commandStreamReceiver = new MockCsrHw<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(commandStreamReceiver);
    std::unique_ptr<ScratchSpaceController> scratchController = std::make_unique<MockScratchSpaceControllerXeHPPlus>(pDevice->getRootDeviceIndex(),
                                                                                                                     *pDevice->executionEnvironment,
                                                                                                                     *commandStreamReceiver->getInternalAllocationStorage());
    bool cfeStateDirty = false;
    bool stateBaseAddressDirty = false;

    void *surfaceHeap = alignedMalloc(0x1000, 0x1000);
    NEO::GraphicsAllocation heap1(1u, NEO::GraphicsAllocation::AllocationType::BUFFER, surfaceHeap, 0u, 0u, 0u, MemoryPool::System4KBPages, 0u);
    NEO::GraphicsAllocation heap2(1u, NEO::GraphicsAllocation::AllocationType::BUFFER, surfaceHeap, 0u, 0u, 0u, MemoryPool::System4KBPages, 0u);
    NEO::GraphicsAllocation heap3(1u, NEO::GraphicsAllocation::AllocationType::BUFFER, surfaceHeap, 0u, 0u, 0u, MemoryPool::System4KBPages, 0u);
    HeapContainer container;

    container.push_back(&heap1);
    container.push_back(&heap2);
    container.push_back(&heap3);

    scratchController->programHeaps(container, 0u, 1u, 0u, 0u, commandStreamReceiver->getOsContext(), stateBaseAddressDirty, cfeStateDirty);

    auto scratch = static_cast<MockScratchSpaceControllerXeHPPlus *>(scratchController.get());
    EXPECT_EQ(scratch->requiredScratchSpaceCalledTimes, 1u);
    EXPECT_EQ(scratch->programSurfaceStateCalledTimes, 2u);

    alignedFree(surfaceHeap);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchWhenSetNewSshPtrAndChangeIdIsFalseThenSlotIdIsNotChanged) {
    class MockScratchSpaceControllerXeHPPlus : public ScratchSpaceControllerXeHPPlus {
      public:
        uint32_t programSurfaceStateCalledTimes = 0u;
        MockScratchSpaceControllerXeHPPlus(uint32_t rootDeviceIndex,
                                           ExecutionEnvironment &environment,
                                           InternalAllocationStorage &allocationStorage) : ScratchSpaceControllerXeHPPlus(rootDeviceIndex, environment, allocationStorage) {}

        using ScratchSpaceControllerXeHPPlus::scratchAllocation;
        using ScratchSpaceControllerXeHPPlus::slotId;

      protected:
        void programSurfaceState() override {
            programSurfaceStateCalledTimes++;
        };
    };

    auto commandStreamReceiver = new MockCsrHw<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(commandStreamReceiver);
    std::unique_ptr<ScratchSpaceController> scratchController = std::make_unique<MockScratchSpaceControllerXeHPPlus>(pDevice->getRootDeviceIndex(),
                                                                                                                     *pDevice->executionEnvironment,
                                                                                                                     *commandStreamReceiver->getInternalAllocationStorage());

    NEO::GraphicsAllocation graphicsAllocation(1u, NEO::GraphicsAllocation::AllocationType::BUFFER, nullptr, 0u, 0u, 0u, MemoryPool::System4KBPages, 0u);

    bool cfeStateDirty = false;

    void *surfaceHeap = alignedMalloc(0x1000, 0x1000);

    auto scratch = static_cast<MockScratchSpaceControllerXeHPPlus *>(scratchController.get());
    scratch->slotId = 10;
    scratch->scratchAllocation = &graphicsAllocation;
    scratch->setNewSshPtr(surfaceHeap, cfeStateDirty, false);
    scratch->scratchAllocation = nullptr;
    EXPECT_EQ(10u, scratch->slotId);
    EXPECT_EQ(scratch->programSurfaceStateCalledTimes, 1u);
    EXPECT_TRUE(cfeStateDirty);

    alignedFree(surfaceHeap);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchWhenProgramSurfaceStateAndUpdateSlotIsFalseThenSlotIdIsNotChanged) {
    class MockScratchSpaceControllerXeHPPlus : public ScratchSpaceControllerXeHPPlus {
      public:
        MockScratchSpaceControllerXeHPPlus(uint32_t rootDeviceIndex,
                                           ExecutionEnvironment &environment,
                                           InternalAllocationStorage &allocationStorage) : ScratchSpaceControllerXeHPPlus(rootDeviceIndex, environment, allocationStorage) {}

        using ScratchSpaceControllerXeHPPlus::programSurfaceState;
        using ScratchSpaceControllerXeHPPlus::scratchAllocation;
        using ScratchSpaceControllerXeHPPlus::slotId;
        using ScratchSpaceControllerXeHPPlus::surfaceStateHeap;
        using ScratchSpaceControllerXeHPPlus::updateSlots;
    };

    auto commandStreamReceiver = new MockCsrHw<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(commandStreamReceiver);
    std::unique_ptr<ScratchSpaceController> scratchController = std::make_unique<MockScratchSpaceControllerXeHPPlus>(pDevice->getRootDeviceIndex(),
                                                                                                                     *pDevice->executionEnvironment,
                                                                                                                     *commandStreamReceiver->getInternalAllocationStorage());

    NEO::GraphicsAllocation graphicsAllocation(1u, NEO::GraphicsAllocation::AllocationType::BUFFER, nullptr, 0u, 0u, 0u, MemoryPool::System4KBPages, 0u);

    void *surfaceHeap = alignedMalloc(0x1000, 0x1000);

    auto scratch = static_cast<MockScratchSpaceControllerXeHPPlus *>(scratchController.get());
    scratch->surfaceStateHeap = static_cast<char *>(surfaceHeap);
    scratch->slotId = 10;
    scratch->updateSlots = false;
    scratch->scratchAllocation = &graphicsAllocation;
    scratch->programSurfaceState();
    scratch->scratchAllocation = nullptr;
    EXPECT_EQ(10u, scratch->slotId);

    alignedFree(surfaceHeap);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceSurfaceStateEnabledWhenBiggerPrivateScratchSpaceRequiredThenReplaceAllocation) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnablePrivateScratchSlot1.set(1);
    RENDER_SURFACE_STATE surfaceState[6];
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(pDevice->getGpgpuCommandStreamReceiver().getOsContext());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver.getScratchSpaceController());

    bool cfeStateDirty = false;
    bool stateBaseAddressDirty = false;

    uint32_t sizeForPrivateScratch = MemoryConstants::pageSize;

    scratchController->setRequiredScratchSpace(surfaceState, 0u, 0u, sizeForPrivateScratch, 0u,
                                               *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_TRUE(cfeStateDirty);
    uint64_t gpuVa = scratchController->privateScratchAllocation->getGpuAddress();
    EXPECT_EQ(gpuVa, surfaceState[3].getSurfaceBaseAddress());

    scratchController->setRequiredScratchSpace(surfaceState, 0u, 0u, sizeForPrivateScratch * 2, 0u,
                                               *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_TRUE(cfeStateDirty);

    EXPECT_NE(gpuVa, scratchController->privateScratchAllocation->getGpuAddress());
    EXPECT_EQ(scratchController->privateScratchAllocation->getGpuAddress(), surfaceState[5].getSurfaceBaseAddress());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceControllerWithOnlyPrivateScratchSpaceWhenGettingPatchAddressThenGetCorrectValue) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnablePrivateScratchSlot1.set(1);
    RENDER_SURFACE_STATE surfaceState[6];
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(pDevice->getGpgpuCommandStreamReceiver().getOsContext());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver.getScratchSpaceController());

    bool cfeStateDirty = false;
    bool stateBaseAddressDirty = false;

    uint32_t sizeForPrivateScratch = MemoryConstants::pageSize;

    EXPECT_EQ(nullptr, scratchController->getScratchSpaceAllocation());
    EXPECT_EQ(nullptr, scratchController->getPrivateScratchSpaceAllocation());

    EXPECT_EQ(0u, scratchController->getScratchPatchAddress());

    scratchController->setRequiredScratchSpace(surfaceState, 0u, 0u, sizeForPrivateScratch, 0u,
                                               *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_TRUE(cfeStateDirty);
    auto expectedPatchAddress = 2 * sizeof(RENDER_SURFACE_STATE);
    EXPECT_EQ(nullptr, scratchController->getScratchSpaceAllocation());
    EXPECT_NE(nullptr, scratchController->getPrivateScratchSpaceAllocation());

    EXPECT_EQ(expectedPatchAddress, scratchController->getScratchPatchAddress());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceSurfaceStateEnabledWhenNotBiggerPrivateScratchSpaceRequiredThenCfeStateIsNotDirty) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnablePrivateScratchSlot1.set(1);
    RENDER_SURFACE_STATE surfaceState[4];
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(pDevice->getGpgpuCommandStreamReceiver().getOsContext());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver.getScratchSpaceController());

    bool cfeStateDirty = false;
    bool stateBaseAddressDirty = false;

    uint32_t sizeForPrivateScratch = MemoryConstants::pageSize;

    scratchController->setRequiredScratchSpace(surfaceState, 0u, 0u, sizeForPrivateScratch, 0u,
                                               *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_TRUE(cfeStateDirty);
    uint64_t gpuVa = scratchController->privateScratchAllocation->getGpuAddress();
    cfeStateDirty = false;

    scratchController->setRequiredScratchSpace(surfaceState, 0u, 0u, sizeForPrivateScratch, 0u,
                                               *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_FALSE(cfeStateDirty);

    EXPECT_EQ(gpuVa, scratchController->privateScratchAllocation->getGpuAddress());
    EXPECT_EQ(gpuVa, surfaceState[3].getSurfaceBaseAddress());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceSurfaceStateWithoutPrivateScratchSpaceWhenDoubleAllocationsScratchSpaceIsUsedThenPrivateScratchAddressIsZero) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnablePrivateScratchSlot1.set(1);
    RENDER_SURFACE_STATE surfaceState[4];
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(pDevice->getGpgpuCommandStreamReceiver().getOsContext());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver.getScratchSpaceController());

    bool cfeStateDirty = false;
    bool stateBaseAddressDirty = false;

    uint32_t sizeForScratch = MemoryConstants::pageSize;

    scratchController->setRequiredScratchSpace(surfaceState, 0u, sizeForScratch, 0u, 0u,
                                               *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_TRUE(cfeStateDirty);
    EXPECT_EQ(nullptr, scratchController->privateScratchAllocation);

    EXPECT_EQ(0u, surfaceState[3].getSurfaceBaseAddress());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceControllerWhenDebugKeyForPrivateScratchIsDisabledThenThereAre16Slots) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnablePrivateScratchSlot1.set(0);
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(pDevice->getGpgpuCommandStreamReceiver().getOsContext());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver.getScratchSpaceController());
    EXPECT_EQ(16u, scratchController->stateSlotsCount);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceControllerWhenDebugKeyForPrivateScratchIsEnabledThenThereAre32Slots) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnablePrivateScratchSlot1.set(1);
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(pDevice->getGpgpuCommandStreamReceiver().getOsContext());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver.getScratchSpaceController());
    EXPECT_EQ(32u, scratchController->stateSlotsCount);
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenScratchSpaceSurfaceStateEnabledWhenSizeForPrivateScratchSpaceIsMisalignedThenAlignItTo64) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnablePrivateScratchSlot1.set(1);
    RENDER_SURFACE_STATE surfaceState[4];
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver.getScratchSpaceController());

    uint32_t misalignedSizeForPrivateScratch = MemoryConstants::pageSize + 1;

    bool cfeStateDirty = false;
    bool stateBaseAddressDirty = false;
    scratchController->setRequiredScratchSpace(surfaceState, 0u, 0u, misalignedSizeForPrivateScratch, 0u,
                                               *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_NE(scratchController->privateScratchSizeBytes, misalignedSizeForPrivateScratch * scratchController->computeUnitsUsedForScratch);
    EXPECT_EQ(scratchController->privateScratchSizeBytes, alignUp(misalignedSizeForPrivateScratch, 64) * scratchController->computeUnitsUsedForScratch);
    EXPECT_EQ(scratchController->privateScratchSizeBytes, scratchController->getPrivateScratchSpaceAllocation()->getUnderlyingBufferSize());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenDisabledPrivateScratchSpaceWhenSizeForPrivateScratchSpaceIsProvidedThenItIsNotCreated) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnablePrivateScratchSlot1.set(0);
    RENDER_SURFACE_STATE surfaceState[4];
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver.getScratchSpaceController());

    bool cfeStateDirty = false;
    bool stateBaseAddressDirty = false;
    scratchController->setRequiredScratchSpace(surfaceState, 0u, MemoryConstants::pageSize, MemoryConstants::pageSize, 0u,
                                               *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_EQ(0u, scratchController->privateScratchSizeBytes);
    EXPECT_EQ(nullptr, scratchController->getPrivateScratchSpaceAllocation());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenDisabledPrivateScratchSpaceWhenGettingOffsetForSlotThenEachSlotContainsOnlyOneSurfaceState) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnablePrivateScratchSlot1.set(0);
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto scratchController = static_cast<MockScratchSpaceControllerXeHPPlus *>(commandStreamReceiver.getScratchSpaceController());
    EXPECT_EQ(sizeof(RENDER_SURFACE_STATE), scratchController->getOffsetToSurfaceState(1u));
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenBlockedCacheFlushCmdWhenSubmittingThenDispatchBlockedCommands) {
    using MI_SEMAPHORE_WAIT = typename FamilyType::MI_SEMAPHORE_WAIT;
    MockContext context(pClDevice);

    auto mockCsr = new MockCsrHw2<FamilyType>(*pDevice->getExecutionEnvironment(), pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(mockCsr);
    mockCsr->timestampPacketWriteEnabled = true;
    mockCsr->storeFlushedTaskStream = true;

    auto cmdQ0 = clUniquePtr(new MockCommandQueueHw<FamilyType>(&context, pClDevice, nullptr));

    auto &secondEngine = pDevice->getEngine(pDevice->getHardwareInfo().capabilityTable.defaultEngineType, EngineUsage::LowPriority);
    static_cast<UltCommandStreamReceiver<FamilyType> *>(secondEngine.commandStreamReceiver)->timestampPacketWriteEnabled = true;

    auto cmdQ1 = clUniquePtr(new MockCommandQueueHw<FamilyType>(&context, pClDevice, nullptr));
    cmdQ1->gpgpuEngine = &secondEngine;
    cmdQ1->timestampPacketContainer = std::make_unique<TimestampPacketContainer>();
    EXPECT_NE(&cmdQ0->getGpgpuCommandStreamReceiver(), &cmdQ1->getGpgpuCommandStreamReceiver());

    MockTimestampPacketContainer node0(*pDevice->getGpgpuCommandStreamReceiver().getTimestampPacketAllocator(), 1);
    MockTimestampPacketContainer node1(*pDevice->getGpgpuCommandStreamReceiver().getTimestampPacketAllocator(), 1);

    Event event0(cmdQ0.get(), 0, 0, 0); // on the same CSR
    event0.addTimestampPacketNodes(node0);
    Event event1(cmdQ1.get(), 0, 0, 0); // on different CSR
    event1.addTimestampPacketNodes(node1);

    uint32_t numEventsOnWaitlist = 3;

    UserEvent userEvent;
    cl_event waitlist[] = {&event0, &event1, &userEvent};

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr(Buffer::create(&context, 0, MemoryConstants::pageSize, nullptr, retVal));
    cl_resource_barrier_descriptor_intel descriptor = {};
    descriptor.mem_object = buffer.get();
    BarrierCommand barrierCommand(cmdQ0.get(), &descriptor, 1);

    cmdQ0->enqueueResourceBarrier(&barrierCommand, numEventsOnWaitlist, waitlist, nullptr);

    userEvent.setStatus(CL_COMPLETE);

    HardwareParse hwParserCsr;
    HardwareParse hwParserCmdQ;
    LinearStream taskStream(mockCsr->storedTaskStream.get(), mockCsr->storedTaskStreamSize);
    taskStream.getSpace(mockCsr->storedTaskStreamSize);
    hwParserCsr.parseCommands<FamilyType>(mockCsr->commandStream, 0);
    hwParserCmdQ.parseCommands<FamilyType>(taskStream, 0);

    {
        auto queueSemaphores = findAll<MI_SEMAPHORE_WAIT *>(hwParserCmdQ.cmdList.begin(), hwParserCmdQ.cmdList.end());
        auto expectedQueueSemaphoresCount = 1u;
        if (UnitTestHelper<FamilyType>::isAdditionalMiSemaphoreWaitRequired(pDevice->getHardwareInfo())) {
            expectedQueueSemaphoresCount += 2;
        }
        EXPECT_EQ(expectedQueueSemaphoresCount, queueSemaphores.size());
        auto semaphoreCmd = genCmdCast<MI_SEMAPHORE_WAIT *>(*(queueSemaphores[0]));
        EXPECT_EQ(semaphoreCmd->getCompareOperation(), MI_SEMAPHORE_WAIT::COMPARE_OPERATION::COMPARE_OPERATION_SAD_NOT_EQUAL_SDD);
        EXPECT_EQ(1u, semaphoreCmd->getSemaphoreDataDword());

        auto dataAddress = TimestampPacketHelper::getContextEndGpuAddress(*node0.getNode(0));
        EXPECT_EQ(dataAddress, semaphoreCmd->getSemaphoreGraphicsAddress());
    }
    {
        auto csrSemaphores = findAll<MI_SEMAPHORE_WAIT *>(hwParserCsr.cmdList.begin(), hwParserCsr.cmdList.end());
        EXPECT_EQ(1u, csrSemaphores.size());
        auto semaphoreCmd = genCmdCast<MI_SEMAPHORE_WAIT *>(*(csrSemaphores[0]));
        EXPECT_EQ(semaphoreCmd->getCompareOperation(), MI_SEMAPHORE_WAIT::COMPARE_OPERATION::COMPARE_OPERATION_SAD_NOT_EQUAL_SDD);
        EXPECT_EQ(1u, semaphoreCmd->getSemaphoreDataDword());

        auto dataAddress = TimestampPacketHelper::getContextEndGpuAddress(*node1.getNode(0));

        EXPECT_EQ(dataAddress, semaphoreCmd->getSemaphoreGraphicsAddress());
    }

    EXPECT_TRUE(mockCsr->passedDispatchFlags.blocking);
    EXPECT_TRUE(mockCsr->passedDispatchFlags.guardCommandBufferWithPipeControl);
    EXPECT_EQ(pDevice->getPreemptionMode(), mockCsr->passedDispatchFlags.preemptionMode);

    cmdQ0->isQueueBlocked();
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, WhenOsContextSupportsMultipleDevicesThenCommandStreamReceiverIsMultiOsContextCapable) {
    uint32_t multiDeviceMask = 0b11;
    uint32_t singleDeviceMask = 0b10;
    std::unique_ptr<OsContext> multiDeviceOsContext(OsContext::create(nullptr, 0u,
                                                                      EngineDescriptorHelper::getDefaultDescriptor({aub_stream::ENGINE_RCS, EngineUsage::Regular}, PreemptionMode::MidThread,
                                                                                                                   multiDeviceMask)));
    std::unique_ptr<OsContext> singleDeviceOsContext(OsContext::create(nullptr, 0u,
                                                                       EngineDescriptorHelper::getDefaultDescriptor({aub_stream::ENGINE_RCS, EngineUsage::Regular}, PreemptionMode::MidThread,
                                                                                                                    singleDeviceMask)));

    EXPECT_EQ(2u, multiDeviceOsContext->getNumSupportedDevices());
    EXPECT_EQ(1u, singleDeviceOsContext->getNumSupportedDevices());

    UltCommandStreamReceiver<FamilyType> commandStreamReceiverMulti(*pDevice->getExecutionEnvironment(), pDevice->getRootDeviceIndex(), multiDeviceMask);
    commandStreamReceiverMulti.callBaseIsMultiOsContextCapable = true;
    EXPECT_TRUE(commandStreamReceiverMulti.isMultiOsContextCapable());
    EXPECT_EQ(2u, commandStreamReceiverMulti.deviceBitfield.count());

    UltCommandStreamReceiver<FamilyType> commandStreamReceiverSingle(*pDevice->getExecutionEnvironment(), pDevice->getRootDeviceIndex(), singleDeviceMask);
    commandStreamReceiverSingle.callBaseIsMultiOsContextCapable = true;
    EXPECT_FALSE(commandStreamReceiverSingle.isMultiOsContextCapable());
    EXPECT_EQ(1u, commandStreamReceiverSingle.deviceBitfield.count());
}

HWTEST2_F(CommandStreamReceiverHwTestXeHPPlus, givenXE_HP_COREDefaultSupportEnabledWhenOsSupportsNewResourceImplicitFlushThenReturnOsSupportValue, IsXeHpCore) {
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(*osContext);

    EXPECT_TRUE(ImplicitFlushSettings<FamilyType>::getSettingForNewResource());

    VariableBackup<bool> defaultSettingForNewResourceBackup(&ImplicitFlushSettings<FamilyType>::getSettingForNewResource(), true);

    if (commandStreamReceiver.getOSInterface()->newResourceImplicitFlush) {
        EXPECT_TRUE(commandStreamReceiver.checkPlatformSupportsNewResourceImplicitFlush());
    } else {
        EXPECT_FALSE(commandStreamReceiver.checkPlatformSupportsNewResourceImplicitFlush());
    }
}

HWTEST2_F(CommandStreamReceiverHwTestXeHPPlus, givenXE_HP_COREDefaultSupportDisabledWhenOsSupportsNewResourceImplicitFlushThenReturnOsSupportValue, IsXeHpCore) {
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(*osContext);

    VariableBackup<bool> defaultSettingForNewResourceBackup(&ImplicitFlushSettings<FamilyType>::getSettingForNewResource(), false);

    EXPECT_FALSE(commandStreamReceiver.checkPlatformSupportsNewResourceImplicitFlush());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenPlatformSupportsImplicitFlushForNewResourceWhenCsrIsMultiContextThenExpectNoSupport) {
    VariableBackup<bool> defaultSettingForNewResourceBackup(&ImplicitFlushSettings<FamilyType>::getSettingForNewResource(), true);

    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(*osContext);
    commandStreamReceiver.multiOsContextCapable = true;

    EXPECT_TRUE(ImplicitFlushSettings<FamilyType>::getSettingForNewResource());
    EXPECT_FALSE(commandStreamReceiver.checkPlatformSupportsNewResourceImplicitFlush());
}

HWTEST2_F(CommandStreamReceiverHwTestXeHPPlus, givenXE_HP_COREDefaultSupportEnabledWhenOsSupportsGpuIdleImplicitFlushThenReturnOsSupportValue, IsXeHpCore) {
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(*osContext);

    EXPECT_TRUE(ImplicitFlushSettings<FamilyType>::getSettingForGpuIdle());

    VariableBackup<bool> defaultSettingForGpuIdleBackup(&ImplicitFlushSettings<FamilyType>::getSettingForGpuIdle(), true);

    if (commandStreamReceiver.getOSInterface()->newResourceImplicitFlush) {
        EXPECT_TRUE(commandStreamReceiver.checkPlatformSupportsGpuIdleImplicitFlush());
    } else {
        EXPECT_FALSE(commandStreamReceiver.checkPlatformSupportsGpuIdleImplicitFlush());
    }
}

HWTEST2_F(CommandStreamReceiverHwTestXeHPPlus, givenXE_HP_COREDefaultSupportDisabledWhenOsSupportsGpuIdleImplicitFlushThenReturnOsSupportValue, IsXeHpCore) {
    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(*osContext);

    VariableBackup<bool> defaultSettingForGpuIdleBackup(&ImplicitFlushSettings<FamilyType>::getSettingForGpuIdle(), false);

    EXPECT_FALSE(commandStreamReceiver.checkPlatformSupportsGpuIdleImplicitFlush());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenPlatformSupportsImplicitFlushForIdleGpuWhenCsrIsMultiContextThenExpectNoSupport) {
    VariableBackup<bool> defaultSettingForGpuIdleBackup(&ImplicitFlushSettings<FamilyType>::getSettingForGpuIdle(), true);

    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(*osContext);

    commandStreamReceiver.multiOsContextCapable = true;

    EXPECT_TRUE(ImplicitFlushSettings<FamilyType>::getSettingForGpuIdle());
    EXPECT_FALSE(commandStreamReceiver.checkPlatformSupportsGpuIdleImplicitFlush());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, givenPlatformSupportsImplicitFlushForIdleGpuWhenCsrIsMultiContextAndDirectSubmissionActiveThenExpectSupportTrue) {
    VariableBackup<bool> defaultSettingForGpuIdleBackup(&ImplicitFlushSettings<FamilyType>::getSettingForGpuIdle(), true);
    VariableBackup<bool> backupOsSettingForGpuIdle(&OSInterface::gpuIdleImplicitFlush, true);

    osContext->setDirectSubmissionActive();

    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    commandStreamReceiver.setupContext(*osContext);

    commandStreamReceiver.multiOsContextCapable = true;

    EXPECT_TRUE(ImplicitFlushSettings<FamilyType>::getSettingForGpuIdle());
    EXPECT_TRUE(commandStreamReceiver.checkPlatformSupportsGpuIdleImplicitFlush());
}

HWCMDTEST_F(IGFX_XE_HP_CORE, CommandStreamReceiverHwTestXeHPPlus, whenCreatingWorkPartitionAllocationThenItsPropertiesAreCorrect) {
    DebugManagerStateRestore restore{};
    DebugManager.flags.EnableStaticPartitioning.set(1);
    DebugManager.flags.EnableLocalMemory.set(1);
    UltDeviceFactory deviceFactory{1, 2};
    MockDevice &rootDevice = *deviceFactory.rootDevices[0];
    CommandStreamReceiver &csr = rootDevice.getGpgpuCommandStreamReceiver();

    StorageInfo workPartitionAllocationStorageInfo = csr.getWorkPartitionAllocation()->storageInfo;
    EXPECT_EQ(rootDevice.getDeviceBitfield(), workPartitionAllocationStorageInfo.memoryBanks);
    EXPECT_EQ(rootDevice.getDeviceBitfield(), workPartitionAllocationStorageInfo.pageTablesVisibility);
    EXPECT_FALSE(workPartitionAllocationStorageInfo.cloningOfPageTables);
    EXPECT_TRUE(workPartitionAllocationStorageInfo.tileInstanced);
}

HWTEST2_F(CommandStreamReceiverHwTestXeHPPlus, givenXeHpWhenRayTracingEnabledThenDoNotAddCommandBatchBuffer, IsXEHP) {

    MockCsrHw<FamilyType> commandStreamReceiver(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto cmdSize = commandStreamReceiver.getCmdSizeForPerDssBackedBuffer(pDevice->getHardwareInfo());
    EXPECT_EQ(0u, cmdSize);
    std::unique_ptr<char> buffer(new char[cmdSize]);

    LinearStream cs(buffer.get(), cmdSize);
    DispatchFlags dispatchFlags = DispatchFlagsHelper::createDefaultDispatchFlags();
    dispatchFlags.usePerDssBackedBuffer = true;

    commandStreamReceiver.programPerDssBackedBuffer(cs, *pDevice, dispatchFlags);
    EXPECT_EQ(0u, cs.getUsed());
}