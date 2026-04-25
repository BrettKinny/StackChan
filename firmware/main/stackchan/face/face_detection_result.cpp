/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_detection_result.h"

namespace stackchan {

FaceDetectionResult& GetFaceDetectionResult()
{
    static FaceDetectionResult instance;
    return instance;
}

}  // namespace stackchan
