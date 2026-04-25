/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "camera_arbiter.h"

namespace stackchan {

CameraArbiter::CameraArbiter()
{
    _mutex = xSemaphoreCreateMutex();
    configASSERT(_mutex);
}

CameraArbiter& CameraArbiter::getInstance()
{
    static CameraArbiter instance;
    return instance;
}

}  // namespace stackchan
