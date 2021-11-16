/*
 * Copyright (C) 2020-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/helpers/state_compute_mode_helper.h"

namespace NEO {

template <>
bool StateComputeModeHelper<XE_HPC_COREFamily>::isStateComputeModeRequired(const CsrSizeRequestFlags &csrSizeRequestFlags, bool isThreadArbitionPolicyProgrammed) {
    return csrSizeRequestFlags.coherencyRequestChanged || csrSizeRequestFlags.numGrfRequiredChanged || isThreadArbitionPolicyProgrammed;
}

} // namespace NEO
