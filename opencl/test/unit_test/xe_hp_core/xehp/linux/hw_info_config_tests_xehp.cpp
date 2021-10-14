/*
 * Copyright (C) 2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/os_interface/os_interface.h"
#include "shared/test/common/helpers/default_hw_info.h"
#include "shared/test/common/libult/linux/drm_mock.h"

#include "opencl/test/unit_test/helpers/gtest_helpers.h"
#include "opencl/test/unit_test/os_interface/linux/hw_info_config_linux_tests.h"

using namespace NEO;

struct HwInfoConfigTestLinuxXeHp : HwInfoConfigTestLinux {
    void SetUp() override {
        HwInfoConfigTestLinux::SetUp();

        drm = new DrmMock(*executionEnvironment->rootDeviceEnvironments[0]);
        osInterface->setDriverModel(std::unique_ptr<DriverModel>(drm));

        drm->storedDeviceID = 0x0201;
        drm->setGtType(GTTYPE_GT4);
    }
};

XEHPTEST_F(HwInfoConfigTestLinuxXeHp, WhenConfiguringHwInfoThenZeroIsReturned) {
    auto hwInfoConfig = HwInfoConfig::get(productFamily);
    int ret = hwInfoConfig->configureHwInfoDrm(&pInHwInfo, &outHwInfo, osInterface);
    EXPECT_EQ(0, ret);
}

XEHPTEST_F(HwInfoConfigTestLinuxXeHp, GivenXeHpSdvWhenConfigureHardwareCustomThenKmdNotifyIsEnabled) {
    HwInfoConfig *hwInfoConfig = HwInfoConfig::get(productFamily);

    hwInfoConfig->configureHardwareCustom(&pInHwInfo, osInterface);
    EXPECT_TRUE(pInHwInfo.capabilityTable.kmdNotifyProperties.enableKmdNotify);
    EXPECT_EQ(20ll, pInHwInfo.capabilityTable.kmdNotifyProperties.delayKmdNotifyMicroseconds);
}