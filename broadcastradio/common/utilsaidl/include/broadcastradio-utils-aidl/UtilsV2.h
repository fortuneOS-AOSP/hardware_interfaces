/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <aidl/android/hardware/broadcastradio/IdentifierType.h>
#include <aidl/android/hardware/broadcastradio/Metadata.h>
#include <aidl/android/hardware/broadcastradio/ProgramIdentifier.h>
#include <aidl/android/hardware/broadcastradio/ProgramInfo.h>
#include <aidl/android/hardware/broadcastradio/ProgramSelector.h>

namespace aidl::android::hardware::broadcastradio {

namespace utils {

bool isValidV2(const ProgramIdentifier& id);
bool isValidV2(const ProgramSelector& sel);
bool isValidMetadataV2(const Metadata& metadata);
std::optional<std::string> getMetadataStringV2(const ProgramInfo& info, const Metadata::Tag& tag);

}  // namespace utils

}  // namespace aidl::android::hardware::broadcastradio
