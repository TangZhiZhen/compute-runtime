/*
 * Copyright (C) 2019-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/device/device.h"

namespace NEO {
class RootDevice;
class SubDevice : public Device {
  public:
    SubDevice(ExecutionEnvironment *executionEnvironment, uint32_t subDeviceIndex, Device &rootDevice);
    void incRefInternal() override;
    unique_ptr_if_unused<Device> decRefInternal() override;

    uint32_t getRootDeviceIndex() const override;

    Device *getRootDevice() const override;

    uint32_t getSubDeviceIndex() const;
    bool isSubDevice() const override { return true; }

  protected:
    uint64_t getGlobalMemorySize(uint32_t deviceBitfield) const override;
    bool subDevicesAllowed() const override { return false; };
    const uint32_t subDeviceIndex;
    RootDevice &rootDevice;
};
} // namespace NEO
