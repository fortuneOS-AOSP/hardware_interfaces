/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "FakeVehicleHardware"
#define FAKE_VEHICLEHARDWARE_DEBUG false  // STOPSHIP if true.

#include "FakeVehicleHardware.h"

#include <DefaultConfig.h>
#include <FakeObd2Frame.h>
#include <JsonFakeValueGenerator.h>
#include <LinearFakeValueGenerator.h>
#include <PropertyUtils.h>
#include <TestPropertyUtils.h>
#include <VehicleHalTypes.h>
#include <VehicleUtils.h>
#include <android-base/parsedouble.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>

#include <dirent.h>
#include <inttypes.h>
#include <sys/types.h>
#include <fstream>
#include <regex>
#include <unordered_set>
#include <vector>

namespace android {
namespace hardware {
namespace automotive {
namespace vehicle {
namespace fake {

namespace {

using ::aidl::android::hardware::automotive::vehicle::GetValueRequest;
using ::aidl::android::hardware::automotive::vehicle::GetValueResult;
using ::aidl::android::hardware::automotive::vehicle::RawPropValues;
using ::aidl::android::hardware::automotive::vehicle::SetValueRequest;
using ::aidl::android::hardware::automotive::vehicle::SetValueResult;
using ::aidl::android::hardware::automotive::vehicle::StatusCode;
using ::aidl::android::hardware::automotive::vehicle::VehicleApPowerStateReport;
using ::aidl::android::hardware::automotive::vehicle::VehicleApPowerStateReq;
using ::aidl::android::hardware::automotive::vehicle::VehicleHwKeyInputAction;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropConfig;
using ::aidl::android::hardware::automotive::vehicle::VehicleProperty;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyGroup;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyStatus;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyType;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropValue;

using ::android::base::EqualsIgnoreCase;
using ::android::base::Error;
using ::android::base::ParseFloat;
using ::android::base::Result;
using ::android::base::ScopedLockAssertion;
using ::android::base::StartsWith;
using ::android::base::StringPrintf;

const char* VENDOR_OVERRIDE_DIR = "/vendor/etc/automotive/vhaloverride/";
const char* OVERRIDE_PROPERTY = "persist.vendor.vhal_init_value_override";

// A list of supported options for "--set" command.
const std::unordered_set<std::string> SET_PROP_OPTIONS = {
        // integer.
        "-i",
        // 64bit integer.
        "-i64",
        // float.
        "-f",
        // string.
        "-s",
        // bytes in hex format, e.g. 0xDEADBEEF.
        "-b",
        // Area id in integer.
        "-a",
        // Timestamp in int64.
        "-t"};

}  // namespace

void FakeVehicleHardware::storePropInitialValue(const defaultconfig::ConfigDeclaration& config) {
    const VehiclePropConfig& vehiclePropConfig = config.config;
    int propId = vehiclePropConfig.prop;

    // A global property will have only a single area
    bool globalProp = isGlobalProp(propId);
    size_t numAreas = globalProp ? 1 : vehiclePropConfig.areaConfigs.size();

    for (size_t i = 0; i < numAreas; i++) {
        int32_t curArea = globalProp ? 0 : vehiclePropConfig.areaConfigs[i].areaId;

        // Create a separate instance for each individual zone
        VehiclePropValue prop = {
                .areaId = curArea,
                .prop = propId,
                .timestamp = elapsedRealtimeNano(),
        };

        if (config.initialAreaValues.empty()) {
            if (config.initialValue == RawPropValues{}) {
                // Skip empty initial values.
                continue;
            }
            prop.value = config.initialValue;
        } else if (auto valueForAreaIt = config.initialAreaValues.find(curArea);
                   valueForAreaIt != config.initialAreaValues.end()) {
            prop.value = valueForAreaIt->second;
        } else {
            ALOGW("failed to get default value for prop 0x%x area 0x%x", propId, curArea);
            continue;
        }

        auto result =
                mServerSidePropStore->writeValue(mValuePool->obtain(prop), /*updateStatus=*/true);
        if (!result.ok()) {
            ALOGE("failed to write default config value, error: %s, status: %d",
                  getErrorMsg(result).c_str(), getIntErrorCode(result));
        }
    }
}

FakeVehicleHardware::FakeVehicleHardware()
    : FakeVehicleHardware(std::make_unique<VehiclePropValuePool>()) {}

FakeVehicleHardware::FakeVehicleHardware(std::unique_ptr<VehiclePropValuePool> valuePool)
    : mValuePool(std::move(valuePool)),
      mServerSidePropStore(new VehiclePropertyStore(mValuePool)),
      mFakeObd2Frame(new obd2frame::FakeObd2Frame(mServerSidePropStore)),
      mFakeUserHal(new FakeUserHal(mValuePool)),
      mRecurrentTimer(new RecurrentTimer()),
      mGeneratorHub(new GeneratorHub(
              [this](const VehiclePropValue& value) { eventFromVehicleBus(value); })),
      mPendingGetValueRequests(this),
      mPendingSetValueRequests(this) {
    init();
}

FakeVehicleHardware::~FakeVehicleHardware() {
    mPendingGetValueRequests.stop();
    mPendingSetValueRequests.stop();
    mGeneratorHub.reset();
}

void FakeVehicleHardware::init() {
    for (auto& it : defaultconfig::getDefaultConfigs()) {
        VehiclePropConfig cfg = it.config;
        VehiclePropertyStore::TokenFunction tokenFunction = nullptr;

        if (cfg.prop == OBD2_FREEZE_FRAME) {
            tokenFunction = [](const VehiclePropValue& propValue) { return propValue.timestamp; };
        }

        mServerSidePropStore->registerProperty(cfg, tokenFunction);
        if (obd2frame::FakeObd2Frame::isDiagnosticProperty(cfg)) {
            // Ignore storing default value for diagnostic property. They have special get/set
            // logic.
            continue;
        }
        storePropInitialValue(it);
    }

    maybeOverrideProperties(VENDOR_OVERRIDE_DIR);

    // OBD2_LIVE_FRAME and OBD2_FREEZE_FRAME must be configured in default configs.
    mFakeObd2Frame->initObd2LiveFrame(*mServerSidePropStore->getConfig(OBD2_LIVE_FRAME).value());
    mFakeObd2Frame->initObd2FreezeFrame(
            *mServerSidePropStore->getConfig(OBD2_FREEZE_FRAME).value());

    mServerSidePropStore->setOnValueChangeCallback(
            [this](const VehiclePropValue& value) { return onValueChangeCallback(value); });
}

std::vector<VehiclePropConfig> FakeVehicleHardware::getAllPropertyConfigs() const {
    return mServerSidePropStore->getAllConfigs();
}

VehiclePropValuePool::RecyclableType FakeVehicleHardware::createApPowerStateReq(
        VehicleApPowerStateReq state) {
    auto req = mValuePool->obtain(VehiclePropertyType::INT32_VEC, 2);
    req->prop = toInt(VehicleProperty::AP_POWER_STATE_REQ);
    req->areaId = 0;
    req->timestamp = elapsedRealtimeNano();
    req->status = VehiclePropertyStatus::AVAILABLE;
    req->value.int32Values[0] = toInt(state);
    // Param = 0.
    req->value.int32Values[1] = 0;
    return req;
}

VhalResult<void> FakeVehicleHardware::setApPowerStateReport(const VehiclePropValue& value) {
    auto updatedValue = mValuePool->obtain(value);
    updatedValue->timestamp = elapsedRealtimeNano();

    if (auto writeResult = mServerSidePropStore->writeValue(std::move(updatedValue));
        !writeResult.ok()) {
        return StatusError(getErrorCode(writeResult))
               << "failed to write value into property store, error: " << getErrorMsg(writeResult);
    }

    VehiclePropValuePool::RecyclableType prop;
    int32_t state = value.value.int32Values[0];
    switch (state) {
        case toInt(VehicleApPowerStateReport::DEEP_SLEEP_EXIT):
            [[fallthrough]];
        case toInt(VehicleApPowerStateReport::HIBERNATION_EXIT):
            [[fallthrough]];
        case toInt(VehicleApPowerStateReport::SHUTDOWN_CANCELLED):
            [[fallthrough]];
        case toInt(VehicleApPowerStateReport::WAIT_FOR_VHAL):
            // CPMS is in WAIT_FOR_VHAL state, simply move to ON and send back to HAL.
            // Must erase existing state because in the case when Car Service crashes, the power
            // state would already be ON when we receive WAIT_FOR_VHAL and thus new property change
            // event would be generated. However, Car Service always expect a property change event
            // even though there is not actual state change.
            mServerSidePropStore->removeValuesForProperty(
                    toInt(VehicleProperty::AP_POWER_STATE_REQ));
            prop = createApPowerStateReq(VehicleApPowerStateReq::ON);

            // ALWAYS update status for generated property value
            if (auto writeResult =
                        mServerSidePropStore->writeValue(std::move(prop), /*updateStatus=*/true);
                !writeResult.ok()) {
                return StatusError(getErrorCode(writeResult))
                       << "failed to write AP_POWER_STATE_REQ into property store, error: "
                       << getErrorMsg(writeResult);
            }
            break;
        case toInt(VehicleApPowerStateReport::DEEP_SLEEP_ENTRY):
            [[fallthrough]];
        case toInt(VehicleApPowerStateReport::HIBERNATION_ENTRY):
            [[fallthrough]];
        case toInt(VehicleApPowerStateReport::SHUTDOWN_START):
            // CPMS is in WAIT_FOR_FINISH state, send the FINISHED command
            // Send back to HAL
            // ALWAYS update status for generated property value
            prop = createApPowerStateReq(VehicleApPowerStateReq::FINISHED);
            if (auto writeResult =
                        mServerSidePropStore->writeValue(std::move(prop), /*updateStatus=*/true);
                !writeResult.ok()) {
                return StatusError(getErrorCode(writeResult))
                       << "failed to write AP_POWER_STATE_REQ into property store, error: "
                       << getErrorMsg(writeResult);
            }
            break;
        default:
            ALOGE("Unknown VehicleApPowerStateReport: %d", state);
            break;
    }
    return {};
}

bool FakeVehicleHardware::isHvacPropAndHvacNotAvailable(int32_t propId) {
    std::unordered_set<int32_t> powerProps(std::begin(HVAC_POWER_PROPERTIES),
                                           std::end(HVAC_POWER_PROPERTIES));
    if (powerProps.count(propId)) {
        auto hvacPowerOnResult =
                mServerSidePropStore->readValue(toInt(VehicleProperty::HVAC_POWER_ON), HVAC_ALL);

        if (hvacPowerOnResult.ok() && hvacPowerOnResult.value()->value.int32Values.size() == 1 &&
            hvacPowerOnResult.value()->value.int32Values[0] == 0) {
            return true;
        }
    }
    return false;
}

VhalResult<void> FakeVehicleHardware::setUserHalProp(const VehiclePropValue& value) {
    auto result = mFakeUserHal->onSetProperty(value);
    if (!result.ok()) {
        return StatusError(getErrorCode(result))
               << "onSetProperty(): HAL returned error: " << getErrorMsg(result);
    }
    auto& updatedValue = result.value();
    if (updatedValue != nullptr) {
        ALOGI("onSetProperty(): updating property returned by HAL: %s",
              updatedValue->toString().c_str());
        if (auto writeResult = mServerSidePropStore->writeValue(std::move(result.value()));
            !writeResult.ok()) {
            return StatusError(getErrorCode(writeResult))
                   << "failed to write value into property store, error: "
                   << getErrorMsg(writeResult);
        }
    }
    return {};
}

FakeVehicleHardware::ValueResultType FakeVehicleHardware::getUserHalProp(
        const VehiclePropValue& value) const {
    auto propId = value.prop;
    ALOGI("get(): getting value for prop %d from User HAL", propId);

    auto result = mFakeUserHal->onGetProperty(value);
    if (!result.ok()) {
        return StatusError(getErrorCode(result))
               << "get(): User HAL returned error: " << getErrorMsg(result);
    } else {
        auto& gotValue = result.value();
        if (gotValue != nullptr) {
            ALOGI("get(): User HAL returned value: %s", gotValue->toString().c_str());
            gotValue->timestamp = elapsedRealtimeNano();
            return result;
        } else {
            return StatusError(StatusCode::INTERNAL_ERROR) << "get(): User HAL returned null value";
        }
    }
}

FakeVehicleHardware::ValueResultType FakeVehicleHardware::maybeGetSpecialValue(
        const VehiclePropValue& value, bool* isSpecialValue) const {
    *isSpecialValue = false;
    int32_t propId = value.prop;
    ValueResultType result;

    if (mFakeUserHal->isSupported(propId)) {
        *isSpecialValue = true;
        return getUserHalProp(value);
    }

    switch (propId) {
        case OBD2_FREEZE_FRAME:
            *isSpecialValue = true;
            result = mFakeObd2Frame->getObd2FreezeFrame(value);
            if (result.ok()) {
                result.value()->timestamp = elapsedRealtimeNano();
            }
            return result;
        case OBD2_FREEZE_FRAME_INFO:
            *isSpecialValue = true;
            result = mFakeObd2Frame->getObd2DtcInfo();
            if (result.ok()) {
                result.value()->timestamp = elapsedRealtimeNano();
            }
            return result;
        case ECHO_REVERSE_BYTES:
            *isSpecialValue = true;
            return getEchoReverseBytes(value);
        default:
            // Do nothing.
            break;
    }

    return nullptr;
}

FakeVehicleHardware::ValueResultType FakeVehicleHardware::getEchoReverseBytes(
        const VehiclePropValue& value) const {
    auto readResult = mServerSidePropStore->readValue(value);
    if (!readResult.ok()) {
        return readResult;
    }
    auto& gotValue = readResult.value();
    gotValue->timestamp = elapsedRealtimeNano();
    std::vector<uint8_t> byteValues = gotValue->value.byteValues;
    size_t byteSize = byteValues.size();
    for (size_t i = 0; i < byteSize; i++) {
        gotValue->value.byteValues[i] = byteValues[byteSize - 1 - i];
    }
    return std::move(gotValue);
}

VhalResult<void> FakeVehicleHardware::maybeSetSpecialValue(const VehiclePropValue& value,
                                                           bool* isSpecialValue) {
    *isSpecialValue = false;
    VehiclePropValuePool::RecyclableType updatedValue;
    int32_t propId = value.prop;

    if (mFakeUserHal->isSupported(propId)) {
        *isSpecialValue = true;
        return setUserHalProp(value);
    }

    if (isHvacPropAndHvacNotAvailable(propId)) {
        *isSpecialValue = true;
        return StatusError(StatusCode::NOT_AVAILABLE) << "hvac not available";
    }

    switch (propId) {
        case toInt(VehicleProperty::AP_POWER_STATE_REPORT):
            *isSpecialValue = true;
            return setApPowerStateReport(value);
        case toInt(VehicleProperty::VEHICLE_MAP_SERVICE):
            // Placeholder for future implementation of VMS property in the default hal. For
            // now, just returns OK; otherwise, hal clients crash with property not supported.
            *isSpecialValue = true;
            return {};
        case OBD2_FREEZE_FRAME_CLEAR:
            *isSpecialValue = true;
            return mFakeObd2Frame->clearObd2FreezeFrames(value);

#ifdef ENABLE_VENDOR_CLUSTER_PROPERTY_FOR_TESTING
        case toInt(VehicleProperty::CLUSTER_REPORT_STATE):
            [[fallthrough]];
        case toInt(VehicleProperty::CLUSTER_REQUEST_DISPLAY):
            [[fallthrough]];
        case toInt(VehicleProperty::CLUSTER_NAVIGATION_STATE):
            [[fallthrough]];
        case VENDOR_CLUSTER_SWITCH_UI:
            [[fallthrough]];
        case VENDOR_CLUSTER_DISPLAY_STATE:
            *isSpecialValue = true;
            updatedValue = mValuePool->obtain(getPropType(value.prop));
            updatedValue->prop = value.prop & ~toInt(VehiclePropertyGroup::MASK);
            if (getPropGroup(value.prop) == VehiclePropertyGroup::SYSTEM) {
                updatedValue->prop |= toInt(VehiclePropertyGroup::VENDOR);
            } else {
                updatedValue->prop |= toInt(VehiclePropertyGroup::SYSTEM);
            }
            updatedValue->value = value.value;
            updatedValue->timestamp = elapsedRealtimeNano();
            updatedValue->areaId = value.areaId;
            if (auto writeResult = mServerSidePropStore->writeValue(std::move(updatedValue));
                !writeResult.ok()) {
                return StatusError(getErrorCode(writeResult))
                       << "failed to write value into property store, error: "
                       << getErrorMsg(writeResult);
            }
            return {};
#endif  // ENABLE_VENDOR_CLUSTER_PROPERTY_FOR_TESTING

        default:
            break;
    }
    return {};
}

StatusCode FakeVehicleHardware::setValues(std::shared_ptr<const SetValuesCallback> callback,
                                          const std::vector<SetValueRequest>& requests) {
    for (auto& request : requests) {
        if (FAKE_VEHICLEHARDWARE_DEBUG) {
            ALOGD("Set value for property ID: %d", request.value.prop);
        }

        // In a real VHAL implementation, you could either send the setValue request to vehicle bus
        // here in the binder thread, or you could send the request in setValue which runs in
        // the handler thread. If you decide to send the setValue request here, you should not
        // wait for the response here and the handler thread should handle the setValue response.
        mPendingSetValueRequests.addRequest(request, callback);
    }

    return StatusCode::OK;
}

VhalResult<void> FakeVehicleHardware::setValue(const VehiclePropValue& value) {
    // In a real VHAL implementation, this will send the request to vehicle bus if not already
    // sent in setValues, and wait for the response from vehicle bus.
    // Here we are just updating mValuePool.
    bool isSpecialValue = false;
    auto setSpecialValueResult = maybeSetSpecialValue(value, &isSpecialValue);

    if (isSpecialValue) {
        if (!setSpecialValueResult.ok()) {
            return StatusError(getErrorCode(setSpecialValueResult))
                   << StringPrintf("failed to set special value for property ID: %d, error: %s",
                                   value.prop, getErrorMsg(setSpecialValueResult).c_str());
        }
        return {};
    }

    auto updatedValue = mValuePool->obtain(value);
    int64_t timestamp = elapsedRealtimeNano();
    updatedValue->timestamp = timestamp;

    auto writeResult = mServerSidePropStore->writeValue(std::move(updatedValue));
    if (!writeResult.ok()) {
        return StatusError(getErrorCode(writeResult))
               << StringPrintf("failed to write value into property store, error: %s",
                               getErrorMsg(writeResult).c_str());
    }

    return {};
}

SetValueResult FakeVehicleHardware::handleSetValueRequest(const SetValueRequest& request) {
    SetValueResult setValueResult;
    setValueResult.requestId = request.requestId;

    if (auto result = setValue(request.value); !result.ok()) {
        ALOGE("failed to set value, error: %s, code: %d", getErrorMsg(result).c_str(),
              getIntErrorCode(result));
        setValueResult.status = getErrorCode(result);
    } else {
        setValueResult.status = StatusCode::OK;
    }

    return setValueResult;
}

StatusCode FakeVehicleHardware::getValues(std::shared_ptr<const GetValuesCallback> callback,
                                          const std::vector<GetValueRequest>& requests) const {
    for (auto& request : requests) {
        if (FAKE_VEHICLEHARDWARE_DEBUG) {
            ALOGD("getValues(%d)", request.prop.prop);
        }

        // In a real VHAL implementation, you could either send the getValue request to vehicle bus
        // here in the binder thread, or you could send the request in getValue which runs in
        // the handler thread. If you decide to send the getValue request here, you should not
        // wait for the response here and the handler thread should handle the getValue response.
        mPendingGetValueRequests.addRequest(request, callback);
    }

    return StatusCode::OK;
}

GetValueResult FakeVehicleHardware::handleGetValueRequest(const GetValueRequest& request) {
    GetValueResult getValueResult;
    getValueResult.requestId = request.requestId;

    auto result = getValue(request.prop);
    if (!result.ok()) {
        ALOGE("failed to get value, error: %s, code: %d", getErrorMsg(result).c_str(),
              getIntErrorCode(result));
        getValueResult.status = getErrorCode(result);
    } else {
        getValueResult.status = StatusCode::OK;
        getValueResult.prop = *result.value();
    }
    return getValueResult;
}

FakeVehicleHardware::ValueResultType FakeVehicleHardware::getValue(
        const VehiclePropValue& value) const {
    // In a real VHAL implementation, this will send the request to vehicle bus if not already
    // sent in getValues, and wait for the response from vehicle bus.
    // Here we are just reading value from mValuePool.
    bool isSpecialValue = false;
    auto result = maybeGetSpecialValue(value, &isSpecialValue);
    if (isSpecialValue) {
        if (!result.ok()) {
            return StatusError(getErrorCode(result))
                   << StringPrintf("failed to get special value: %d, error: %s", value.prop,
                                   getErrorMsg(result).c_str());
        } else {
            return std::move(result);
        }
    }

    auto readResult = mServerSidePropStore->readValue(value);
    if (!readResult.ok()) {
        StatusCode errorCode = getErrorCode(readResult);
        if (errorCode == StatusCode::NOT_AVAILABLE) {
            return StatusError(errorCode) << "value has not been set yet";
        } else {
            return StatusError(errorCode)
                   << "failed to get value, error: " << getErrorMsg(readResult);
        }
    }

    return std::move(readResult);
}

DumpResult FakeVehicleHardware::dump(const std::vector<std::string>& options) {
    DumpResult result;
    result.callerShouldDumpState = false;
    if (options.size() == 0) {
        // We only want caller to dump default state when there is no options.
        result.callerShouldDumpState = true;
        result.buffer = dumpAllProperties();
        return result;
    }
    std::string option = options[0];
    if (EqualsIgnoreCase(option, "--help")) {
        result.buffer = dumpHelp();
        return result;
    } else if (EqualsIgnoreCase(option, "--list")) {
        result.buffer = dumpListProperties();
    } else if (EqualsIgnoreCase(option, "--get")) {
        result.buffer = dumpSpecificProperty(options);
    } else if (EqualsIgnoreCase(option, "--getWithArg")) {
        result.buffer = dumpGetPropertyWithArg(options);
    } else if (EqualsIgnoreCase(option, "--set")) {
        result.buffer = dumpSetProperties(options);
    } else if (EqualsIgnoreCase(option, "--save-prop")) {
        result.buffer = dumpSaveProperty(options);
    } else if (EqualsIgnoreCase(option, "--restore-prop")) {
        result.buffer = dumpRestoreProperty(options);
    } else if (EqualsIgnoreCase(option, "--inject-event")) {
        result.buffer = dumpInjectEvent(options);
    } else if (EqualsIgnoreCase(option, kUserHalDumpOption)) {
        if (options.size() == 1) {
            result.buffer = mFakeUserHal->showDumpHelp();
        } else {
            result.buffer = mFakeUserHal->dump(options[1]);
        }
    } else if (EqualsIgnoreCase(option, "--genfakedata")) {
        result.buffer = genFakeDataCommand(options);
    } else {
        result.buffer = StringPrintf("Invalid option: %s\n", option.c_str());
    }
    return result;
}

std::string FakeVehicleHardware::genFakeDataHelp() {
    return R"(
Generate Fake Data Usage:
--genfakedata --startlinear [propID] [mValue] [cValue] [dispersion] [increment] [interval]: "
Start a linear generator that generates event with floatValue within range:
[mValue - disperson, mValue + dispersion].
propID(int32): ID for the property to generate event for.
mValue(float): The middle of the possible values for the property.
cValue(float): The start value for the property, must be within the range.
dispersion(float): The range the value can change.
increment(float): The step the value would increase by for each generated event,
if exceed the range, the value would loop back.
interval(int64): The interval in nanoseconds the event would generate by.

--genfakedata --stoplinear [propID(int32)]: Stop a linear generator

--genfakedata --startjson --path [jsonFilePath] [repetition]:
Start a JSON generator that would generate events according to a JSON file.
jsonFilePath(string): The path to a JSON file. The JSON content must be in the format of
[{
    "timestamp": 1000000,
    "areaId": 0,
    "value": 8,
    "prop": 289408000
}, {...}]
Each event in the JSON file would be generated by the same interval their timestamp is relative to
the first event's timestamp.
repetition(int32, optional): how many iterations the events would be generated. If it is not
provided, it would iterate indefinitely.

--genfakedata --startjson --content [jsonContent]: Start a JSON generator using the content.

--genfakedata --stopjson [generatorID(string)]: Stop a JSON generator.

--genfakedata --keypress [keyCode(int32)] [display[int32]]: Generate key press.

)";
}

std::string FakeVehicleHardware::parseErrMsg(std::string fieldName, std::string value,
                                             std::string type) {
    return StringPrintf("failed to parse %s as %s: \"%s\"\n%s", fieldName.c_str(), type.c_str(),
                        value.c_str(), genFakeDataHelp().c_str());
}

std::string FakeVehicleHardware::genFakeDataCommand(const std::vector<std::string>& options) {
    if (options.size() < 2) {
        return "No subcommand specified for genfakedata\n" + genFakeDataHelp();
    }

    std::string command = options[1];
    if (command == "--startlinear") {
        // --genfakedata --startlinear [propID(int32)] [middleValue(float)]
        // [currentValue(float)] [dispersion(float)] [increment(float)] [interval(int64)]
        if (options.size() != 8) {
            return "incorrect argument count, need 8 arguments for --genfakedata --startlinear\n" +
                   genFakeDataHelp();
        }
        int32_t propId;
        float middleValue;
        float currentValue;
        float dispersion;
        float increment;
        int64_t interval;
        if (!android::base::ParseInt(options[2], &propId)) {
            return parseErrMsg("propId", options[2], "int");
        }
        if (!android::base::ParseFloat(options[3], &middleValue)) {
            return parseErrMsg("middleValue", options[3], "float");
        }
        if (!android::base::ParseFloat(options[4], &currentValue)) {
            return parseErrMsg("currentValue", options[4], "float");
        }
        if (!android::base::ParseFloat(options[5], &dispersion)) {
            return parseErrMsg("dispersion", options[5], "float");
        }
        if (!android::base::ParseFloat(options[6], &increment)) {
            return parseErrMsg("increment", options[6], "float");
        }
        if (!android::base::ParseInt(options[7], &interval)) {
            return parseErrMsg("interval", options[7], "int");
        }
        auto generator = std::make_unique<LinearFakeValueGenerator>(
                propId, middleValue, currentValue, dispersion, increment, interval);
        mGeneratorHub->registerGenerator(propId, std::move(generator));
        return "Linear event generator started successfully";
    } else if (command == "--stoplinear") {
        // --genfakedata --stoplinear [propID(int32)]
        if (options.size() != 3) {
            return "incorrect argument count, need 3 arguments for --genfakedata --stoplinear\n" +
                   genFakeDataHelp();
        }
        int32_t propId;
        if (!android::base::ParseInt(options[2], &propId)) {
            return parseErrMsg("propId", options[2], "int");
        }
        if (mGeneratorHub->unregisterGenerator(propId)) {
            return "Linear event generator stopped successfully";
        }
        return StringPrintf("No linear event generator found for property: %d", propId);
    } else if (command == "--startjson") {
        // --genfakedata --startjson --path path repetition
        // or
        // --genfakedata --startjson --content content repetition.
        if (options.size() != 4 && options.size() != 5) {
            return "incorrect argument count, need 4 or 5 arguments for --genfakedata "
                   "--startjson\n";
        }
        // Iterate infinitely if repetition number is not provided
        int32_t repetition = -1;
        if (options.size() == 5) {
            if (!android::base::ParseInt(options[4], &repetition)) {
                return parseErrMsg("repetition", options[4], "int");
            }
        }
        std::unique_ptr<JsonFakeValueGenerator> generator;
        if (options[2] == "--path") {
            const std::string& fileName = options[3];
            generator = std::make_unique<JsonFakeValueGenerator>(fileName, repetition);
            if (!generator->hasNext()) {
                return "invalid JSON file, no events";
            }
        } else if (options[2] == "--content") {
            const std::string& content = options[3];
            generator =
                    std::make_unique<JsonFakeValueGenerator>(/*unused=*/true, content, repetition);
            if (!generator->hasNext()) {
                return "invalid JSON content, no events";
            }
        }
        int32_t cookie = std::hash<std::string>()(options[3]);
        mGeneratorHub->registerGenerator(cookie, std::move(generator));
        return StringPrintf("JSON event generator started successfully, ID: %" PRId32, cookie);
    } else if (command == "--stopjson") {
        // --genfakedata --stopjson [generatorID(string)]
        if (options.size() != 3) {
            return "incorrect argument count, need 3 arguments for --genfakedata --stopjson\n";
        }
        int32_t cookie;
        if (!android::base::ParseInt(options[2], &cookie)) {
            return parseErrMsg("cookie", options[2], "int");
        }
        if (mGeneratorHub->unregisterGenerator(cookie)) {
            return "JSON event generator stopped successfully";
        } else {
            return StringPrintf("No JSON event generator found for ID: %s", options[2].c_str());
        }
    } else if (command == "--keypress") {
        int32_t keyCode;
        int32_t display;
        // --genfakedata --keypress [keyCode(int32)] [display[int32]]
        if (options.size() != 4) {
            return "incorrect argument count, need 4 arguments for --genfakedata --keypress\n";
        }
        if (!android::base::ParseInt(options[2], &keyCode)) {
            return parseErrMsg("keyCode", options[2], "int");
        }
        if (!android::base::ParseInt(options[3], &display)) {
            return parseErrMsg("display", options[3], "int");
        }
        // Send back to HAL
        onValueChangeCallback(
                createHwInputKeyProp(VehicleHwKeyInputAction::ACTION_DOWN, keyCode, display));
        onValueChangeCallback(
                createHwInputKeyProp(VehicleHwKeyInputAction::ACTION_UP, keyCode, display));
        return "keypress event generated successfully";
    }

    return StringPrintf("Unknown command: \"%s\"\n%s", command.c_str(), genFakeDataHelp().c_str());
}

VehiclePropValue FakeVehicleHardware::createHwInputKeyProp(VehicleHwKeyInputAction action,
                                                           int32_t keyCode, int32_t targetDisplay) {
    VehiclePropValue value = {
            .prop = toInt(VehicleProperty::HW_KEY_INPUT),
            .areaId = 0,
            .timestamp = elapsedRealtimeNano(),
            .status = VehiclePropertyStatus::AVAILABLE,
            .value.int32Values = {toInt(action), keyCode, targetDisplay},
    };
    return value;
}

void FakeVehicleHardware::eventFromVehicleBus(const VehiclePropValue& value) {
    mServerSidePropStore->writeValue(mValuePool->obtain(value));
}

std::string FakeVehicleHardware::dumpHelp() {
    return "Usage: \n\n"
           "[no args]: dumps (id and value) all supported properties \n"
           "--help: shows this help\n"
           "--list: lists the ids of all supported properties\n"
           "--get <PROP1> [PROP2] [PROPN]: dumps the value of specific properties. \n"
           "--getWithArg <PROP> [ValueArguments]: gets the value for a specific property with "
           "arguments. \n"
           "--set <PROP> [ValueArguments]: sets the value of property PROP. \n"
           "--save-prop <prop> [-a AREA_ID]: saves the current value for PROP, integration test"
           " that modifies prop value must call this before test and restore-prop after test. \n"
           "--restore-prop <prop> [-a AREA_ID]: restores a previously saved property value. \n"
           "--inject-event <PROP> [ValueArguments]: inject a property update event from car\n\n"
           "ValueArguments are in the format of [-i INT_VALUE [INT_VALUE ...]] "
           "[-i64 INT64_VALUE [INT64_VALUE ...]] [-f FLOAT_VALUE [FLOAT_VALUE ...]] [-s STR_VALUE] "
           "[-b BYTES_VALUE] [-a AREA_ID].\n"
           "Notice that the string, bytes and area value can be set just once, while the other can"
           " have multiple values (so they're used in the respective array), "
           "BYTES_VALUE is in the form of 0xXXXX, e.g. 0xdeadbeef.\n" +
           genFakeDataHelp() + "Fake user HAL usage: \n" + mFakeUserHal->showDumpHelp();
}

std::string FakeVehicleHardware::dumpAllProperties() {
    auto configs = mServerSidePropStore->getAllConfigs();
    if (configs.size() == 0) {
        return "no properties to dump\n";
    }
    std::string msg = StringPrintf("dumping %zu properties\n", configs.size());
    int rowNumber = 1;
    for (const VehiclePropConfig& config : configs) {
        msg += dumpOnePropertyByConfig(rowNumber++, config);
    }
    return msg;
}

std::string FakeVehicleHardware::dumpOnePropertyByConfig(int rowNumber,
                                                         const VehiclePropConfig& config) {
    size_t numberAreas = config.areaConfigs.size();
    std::string msg = "";
    if (numberAreas == 0) {
        msg += StringPrintf("%d: ", rowNumber);
        msg += dumpOnePropertyById(config.prop, /* areaId= */ 0);
        return msg;
    }
    for (size_t j = 0; j < numberAreas; ++j) {
        if (numberAreas > 1) {
            msg += StringPrintf("%d-%zu: ", rowNumber, j);
        } else {
            msg += StringPrintf("%d: ", rowNumber);
        }
        msg += dumpOnePropertyById(config.prop, config.areaConfigs[j].areaId);
    }
    return msg;
}

std::string FakeVehicleHardware::dumpOnePropertyById(int32_t propId, int32_t areaId) {
    VehiclePropValue value = {
            .prop = propId,
            .areaId = areaId,
    };
    bool isSpecialValue = false;
    auto result = maybeGetSpecialValue(value, &isSpecialValue);
    if (!isSpecialValue) {
        result = mServerSidePropStore->readValue(value);
    }
    if (!result.ok()) {
        return StringPrintf("failed to read property value: %d, error: %s, code: %d\n", propId,
                            getErrorMsg(result).c_str(), getIntErrorCode(result));

    } else {
        return result.value()->toString() + "\n";
    }
}

std::string FakeVehicleHardware::dumpListProperties() {
    auto configs = mServerSidePropStore->getAllConfigs();
    if (configs.size() == 0) {
        return "no properties to list\n";
    }
    int rowNumber = 1;
    std::string msg = StringPrintf("listing %zu properties\n", configs.size());
    for (const auto& config : configs) {
        msg += StringPrintf("%d: %d\n", rowNumber++, config.prop);
    }
    return msg;
}

Result<void> FakeVehicleHardware::checkArgumentsSize(const std::vector<std::string>& options,
                                                     size_t minSize) {
    size_t size = options.size();
    if (size >= minSize) {
        return {};
    }
    return Error() << StringPrintf("Invalid number of arguments: required at least %zu, got %zu\n",
                                   minSize, size);
}

std::string FakeVehicleHardware::dumpSpecificProperty(const std::vector<std::string>& options) {
    if (auto result = checkArgumentsSize(options, /*minSize=*/2); !result.ok()) {
        return getErrorMsg(result);
    }

    // options[0] is the command itself...
    int rowNumber = 1;
    size_t size = options.size();
    std::string msg = "";
    for (size_t i = 1; i < size; ++i) {
        auto propResult = safelyParseInt<int32_t>(i, options[i]);
        if (!propResult.ok()) {
            msg += getErrorMsg(propResult);
            continue;
        }
        int32_t prop = propResult.value();
        auto result = mServerSidePropStore->getConfig(prop);
        if (!result.ok()) {
            msg += StringPrintf("No property %d\n", prop);
            continue;
        }
        msg += dumpOnePropertyByConfig(rowNumber++, *result.value());
    }
    return msg;
}

std::vector<std::string> FakeVehicleHardware::getOptionValues(
        const std::vector<std::string>& options, size_t* index) {
    std::vector<std::string> values;
    while (*index < options.size()) {
        std::string option = options[*index];
        if (SET_PROP_OPTIONS.find(option) != SET_PROP_OPTIONS.end()) {
            return std::move(values);
        }
        values.push_back(option);
        (*index)++;
    }
    return std::move(values);
}

Result<VehiclePropValue> FakeVehicleHardware::parsePropOptions(
        const std::vector<std::string>& options) {
    // Options format:
    // --set/get/inject-event PROP [-f f1 f2...] [-i i1 i2...] [-i64 i1 i2...] [-s s1 s2...]
    // [-b b1 b2...] [-a a] [-t timestamp]
    size_t optionIndex = 1;
    auto result = safelyParseInt<int32_t>(optionIndex, options[optionIndex]);
    if (!result.ok()) {
        return Error() << StringPrintf("Property value: \"%s\" is not a valid int: %s\n",
                                       options[optionIndex].c_str(), getErrorMsg(result).c_str());
    }
    VehiclePropValue prop = {};
    prop.prop = result.value();
    prop.status = VehiclePropertyStatus::AVAILABLE;
    optionIndex++;
    std::unordered_set<std::string> parsedOptions;

    while (optionIndex < options.size()) {
        std::string argType = options[optionIndex];
        optionIndex++;

        size_t currentIndex = optionIndex;
        std::vector<std::string> argValues = getOptionValues(options, &optionIndex);
        if (parsedOptions.find(argType) != parsedOptions.end()) {
            return Error() << StringPrintf("Duplicate \"%s\" options\n", argType.c_str());
        }
        parsedOptions.insert(argType);
        size_t argValuesSize = argValues.size();
        if (EqualsIgnoreCase(argType, "-i")) {
            if (argValuesSize == 0) {
                return Error() << "No values specified when using \"-i\"\n";
            }
            prop.value.int32Values.resize(argValuesSize);
            for (size_t i = 0; i < argValuesSize; i++) {
                auto int32Result = safelyParseInt<int32_t>(currentIndex + i, argValues[i]);
                if (!int32Result.ok()) {
                    return Error()
                           << StringPrintf("Value: \"%s\" is not a valid int: %s\n",
                                           argValues[i].c_str(), getErrorMsg(int32Result).c_str());
                }
                prop.value.int32Values[i] = int32Result.value();
            }
        } else if (EqualsIgnoreCase(argType, "-i64")) {
            if (argValuesSize == 0) {
                return Error() << "No values specified when using \"-i64\"\n";
            }
            prop.value.int64Values.resize(argValuesSize);
            for (size_t i = 0; i < argValuesSize; i++) {
                auto int64Result = safelyParseInt<int64_t>(currentIndex + i, argValues[i]);
                if (!int64Result.ok()) {
                    return Error()
                           << StringPrintf("Value: \"%s\" is not a valid int64: %s\n",
                                           argValues[i].c_str(), getErrorMsg(int64Result).c_str());
                }
                prop.value.int64Values[i] = int64Result.value();
            }
        } else if (EqualsIgnoreCase(argType, "-f")) {
            if (argValuesSize == 0) {
                return Error() << "No values specified when using \"-f\"\n";
            }
            prop.value.floatValues.resize(argValuesSize);
            for (size_t i = 0; i < argValuesSize; i++) {
                auto floatResult = safelyParseFloat(currentIndex + i, argValues[i]);
                if (!floatResult.ok()) {
                    return Error()
                           << StringPrintf("Value: \"%s\" is not a valid float: %s\n",
                                           argValues[i].c_str(), getErrorMsg(floatResult).c_str());
                }
                prop.value.floatValues[i] = floatResult.value();
            }
        } else if (EqualsIgnoreCase(argType, "-s")) {
            if (argValuesSize != 1) {
                return Error() << "Expect exact one value when using \"-s\"\n";
            }
            prop.value.stringValue = argValues[0];
        } else if (EqualsIgnoreCase(argType, "-b")) {
            if (argValuesSize != 1) {
                return Error() << "Expect exact one value when using \"-b\"\n";
            }
            auto bytesResult = parseHexString(argValues[0]);
            if (!bytesResult.ok()) {
                return Error() << StringPrintf("value: \"%s\" is not a valid hex string: %s\n",
                                               argValues[0].c_str(),
                                               getErrorMsg(bytesResult).c_str());
            }
            prop.value.byteValues = std::move(bytesResult.value());
        } else if (EqualsIgnoreCase(argType, "-a")) {
            if (argValuesSize != 1) {
                return Error() << "Expect exact one value when using \"-a\"\n";
            }
            auto int32Result = safelyParseInt<int32_t>(currentIndex, argValues[0]);
            if (!int32Result.ok()) {
                return Error() << StringPrintf("Area ID: \"%s\" is not a valid int: %s\n",
                                               argValues[0].c_str(),
                                               getErrorMsg(int32Result).c_str());
            }
            prop.areaId = int32Result.value();
        } else if (EqualsIgnoreCase(argType, "-t")) {
            if (argValuesSize != 1) {
                return Error() << "Expect exact one value when using \"-t\"\n";
            }
            auto int64Result = safelyParseInt<int64_t>(currentIndex, argValues[0]);
            if (!int64Result.ok()) {
                return Error() << StringPrintf("Timestamp: \"%s\" is not a valid int64: %s\n",
                                               argValues[0].c_str(),
                                               getErrorMsg(int64Result).c_str());
            }
            prop.timestamp = int64Result.value();
        } else {
            return Error() << StringPrintf("Unknown option: %s\n", argType.c_str());
        }
    }

    return prop;
}

std::string FakeVehicleHardware::dumpSetProperties(const std::vector<std::string>& options) {
    if (auto result = checkArgumentsSize(options, 3); !result.ok()) {
        return getErrorMsg(result);
    }

    auto parseResult = parsePropOptions(options);
    if (!parseResult.ok()) {
        return getErrorMsg(parseResult);
    }
    VehiclePropValue prop = std::move(parseResult.value());
    ALOGD("Dump: Setting property: %s", prop.toString().c_str());

    bool isSpecialValue = false;
    auto setResult = maybeSetSpecialValue(prop, &isSpecialValue);

    if (!isSpecialValue) {
        auto updatedValue = mValuePool->obtain(prop);
        updatedValue->timestamp = elapsedRealtimeNano();
        setResult = mServerSidePropStore->writeValue(std::move(updatedValue));
    }

    if (setResult.ok()) {
        return StringPrintf("Set property: %s\n", prop.toString().c_str());
    }
    return StringPrintf("failed to set property: %s, error: %s\n", prop.toString().c_str(),
                        getErrorMsg(setResult).c_str());
}

std::string FakeVehicleHardware::dumpGetPropertyWithArg(const std::vector<std::string>& options) {
    if (auto result = checkArgumentsSize(options, 3); !result.ok()) {
        return getErrorMsg(result);
    }

    auto parseResult = parsePropOptions(options);
    if (!parseResult.ok()) {
        return getErrorMsg(parseResult);
    }
    VehiclePropValue prop = std::move(parseResult.value());
    ALOGD("Dump: Getting property: %s", prop.toString().c_str());

    bool isSpecialValue = false;
    auto result = maybeGetSpecialValue(prop, &isSpecialValue);

    if (!isSpecialValue) {
        result = mServerSidePropStore->readValue(prop);
    }

    if (!result.ok()) {
        return StringPrintf("failed to read property value: %d, error: %s, code: %d\n", prop.prop,
                            getErrorMsg(result).c_str(), getIntErrorCode(result));
    }
    return StringPrintf("Get property result: %s\n", result.value()->toString().c_str());
}

std::string FakeVehicleHardware::dumpSaveProperty(const std::vector<std::string>& options) {
    // Format: --save-prop PROP [-a areaID]
    if (auto result = checkArgumentsSize(options, 2); !result.ok()) {
        return getErrorMsg(result);
    }

    auto parseResult = parsePropOptions(options);
    if (!parseResult.ok()) {
        return getErrorMsg(parseResult);
    }
    // We are only using the prop and areaId option.
    VehiclePropValue value = std::move(parseResult.value());
    int32_t propId = value.prop;
    int32_t areaId = value.areaId;

    auto readResult = mServerSidePropStore->readValue(value);
    if (!readResult.ok()) {
        return StringPrintf("Failed to save current property value, error: %s",
                            getErrorMsg(readResult).c_str());
    }

    std::scoped_lock<std::mutex> lockGuard(mLock);
    mSavedProps[PropIdAreaId{
            .propId = propId,
            .areaId = areaId,
    }] = std::move(readResult.value());

    return StringPrintf("Property: %" PRId32 ", areaID: %" PRId32 " saved", propId, areaId);
}

std::string FakeVehicleHardware::dumpRestoreProperty(const std::vector<std::string>& options) {
    // Format: --restore-prop PROP [-a areaID]
    if (auto result = checkArgumentsSize(options, 2); !result.ok()) {
        return getErrorMsg(result);
    }

    auto parseResult = parsePropOptions(options);
    if (!parseResult.ok()) {
        return getErrorMsg(parseResult);
    }
    // We are only using the prop and areaId option.
    VehiclePropValue value = std::move(parseResult.value());
    int32_t propId = value.prop;
    int32_t areaId = value.areaId;
    VehiclePropValuePool::RecyclableType savedValue;

    {
        std::scoped_lock<std::mutex> lockGuard(mLock);
        auto it = mSavedProps.find(PropIdAreaId{
                .propId = propId,
                .areaId = areaId,
        });
        if (it == mSavedProps.end()) {
            return StringPrintf("No saved property for property: %" PRId32 ", areaID: %" PRId32,
                                propId, areaId);
        }

        savedValue = std::move(it->second);
        // Remove the saved property after restoring it.
        mSavedProps.erase(it);
    }

    // Update timestamp.
    savedValue->timestamp = elapsedRealtimeNano();

    auto writeResult = mServerSidePropStore->writeValue(std::move(savedValue));
    if (!writeResult.ok()) {
        return StringPrintf("Failed to restore property value, error: %s",
                            getErrorMsg(writeResult).c_str());
    }

    return StringPrintf("Property: %" PRId32 ", areaID: %" PRId32 " restored", propId, areaId);
}

std::string FakeVehicleHardware::dumpInjectEvent(const std::vector<std::string>& options) {
    if (auto result = checkArgumentsSize(options, 3); !result.ok()) {
        return getErrorMsg(result);
    }

    auto parseResult = parsePropOptions(options);
    if (!parseResult.ok()) {
        return getErrorMsg(parseResult);
    }
    VehiclePropValue prop = std::move(parseResult.value());
    ALOGD("Dump: Injecting event from vehicle bus: %s", prop.toString().c_str());

    eventFromVehicleBus(prop);

    return StringPrintf("Event for property: %d injected", prop.prop);
}

StatusCode FakeVehicleHardware::checkHealth() {
    // Always return OK for checkHealth.
    return StatusCode::OK;
}

void FakeVehicleHardware::registerOnPropertyChangeEvent(
        std::unique_ptr<const PropertyChangeCallback> callback) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    mOnPropertyChangeCallback = std::move(callback);
}

void FakeVehicleHardware::registerOnPropertySetErrorEvent(
        std::unique_ptr<const PropertySetErrorCallback> callback) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    mOnPropertySetErrorCallback = std::move(callback);
}

StatusCode FakeVehicleHardware::updateSampleRate(int32_t propId, int32_t areaId, float sampleRate) {
    // DefaultVehicleHal makes sure that sampleRate must be within minSampleRate and maxSampleRate.
    // For fake implementation, we would write the same value with a new timestamp into propStore
    // at sample rate.
    std::scoped_lock<std::mutex> lockGuard(mLock);

    PropIdAreaId propIdAreaId{
            .propId = propId,
            .areaId = areaId,
    };
    if (mRecurrentActions.find(propIdAreaId) != mRecurrentActions.end()) {
        mRecurrentTimer->unregisterTimerCallback(mRecurrentActions[propIdAreaId]);
    }
    if (sampleRate == 0) {
        return StatusCode::OK;
    }
    int64_t interval = static_cast<int64_t>(1'000'000'000. / sampleRate);
    auto action = std::make_shared<RecurrentTimer::Callback>([this, propId, areaId] {
        // Refresh the property value. In real implementation, this should poll the latest value
        // from vehicle bus. Here, we are just refreshing the existing value with a new timestamp.
        auto result = getValue(VehiclePropValue{
                .prop = propId,
                .areaId = areaId,
        });
        if (!result.ok()) {
            // Failed to read current value, skip refreshing.
            return;
        }
        result.value()->timestamp = elapsedRealtimeNano();
        // Must remove the value before writing, otherwise, we would generate no update event since
        // the value is the same.
        mServerSidePropStore->removeValue(*result.value());
        mServerSidePropStore->writeValue(std::move(result.value()));
    });
    mRecurrentTimer->registerTimerCallback(interval, action);
    mRecurrentActions[propIdAreaId] = action;
    return StatusCode::OK;
}

void FakeVehicleHardware::onValueChangeCallback(const VehiclePropValue& value) {
    std::scoped_lock<std::mutex> lockGuard(mLock);

    if (mOnPropertyChangeCallback == nullptr) {
        return;
    }

    std::vector<VehiclePropValue> updatedValues;
    updatedValues.push_back(value);
    (*mOnPropertyChangeCallback)(std::move(updatedValues));
}

void FakeVehicleHardware::maybeOverrideProperties(const char* overrideDir) {
    if (android::base::GetBoolProperty(OVERRIDE_PROPERTY, false)) {
        overrideProperties(overrideDir);
    }
}

void FakeVehicleHardware::overrideProperties(const char* overrideDir) {
    ALOGI("loading vendor override properties from %s", overrideDir);
    if (auto dir = opendir(overrideDir); dir != NULL) {
        std::regex regJson(".*[.]json", std::regex::icase);
        while (auto f = readdir(dir)) {
            if (!std::regex_match(f->d_name, regJson)) {
                continue;
            }
            std::string file = overrideDir + std::string(f->d_name);
            JsonFakeValueGenerator tmpGenerator(file);

            std::vector<VehiclePropValue> propValues = tmpGenerator.getAllEvents();
            for (const VehiclePropValue& prop : propValues) {
                auto propToStore = mValuePool->obtain(prop);
                propToStore->timestamp = elapsedRealtimeNano();
                if (auto result = mServerSidePropStore->writeValue(std::move(propToStore),
                                                                   /*updateStatus=*/true);
                    !result.ok()) {
                    ALOGW("failed to write vendor override properties: %d, error: %s, code: %d",
                          prop.prop, getErrorMsg(result).c_str(), getIntErrorCode(result));
                }
            }
        }
        closedir(dir);
    }
}

Result<float> FakeVehicleHardware::safelyParseFloat(int index, const std::string& s) {
    float out;
    if (!ParseFloat(s, &out)) {
        return Error() << StringPrintf("non-float argument at index %d: %s\n", index, s.c_str());
    }
    return out;
}

Result<std::vector<uint8_t>> FakeVehicleHardware::parseHexString(const std::string& s) {
    std::vector<uint8_t> bytes;
    if (s.size() % 2 != 0) {
        return Error() << StringPrintf("invalid hex string: %s, should have even size\n",
                                       s.c_str());
    }
    if (!StartsWith(s, "0x")) {
        return Error() << StringPrintf("hex string should start with \"0x\", got %s\n", s.c_str());
    }
    std::string subs = s.substr(2);
    std::transform(subs.begin(), subs.end(), subs.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    bool highDigit = true;
    for (size_t i = 0; i < subs.size(); i++) {
        char c = subs[i];
        uint8_t v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else {
            return Error() << StringPrintf("invalid character %c in hex string %s\n", c,
                                           subs.c_str());
        }
        if (highDigit) {
            bytes.push_back(v * 16);
        } else {
            bytes[bytes.size() - 1] += v;
        }
        highDigit = !highDigit;
    }
    return bytes;
}

template <class CallbackType, class RequestType>
FakeVehicleHardware::PendingRequestHandler<CallbackType, RequestType>::PendingRequestHandler(
        FakeVehicleHardware* hardware)
    : mHardware(hardware), mThread([this] {
          while (mRequests.waitForItems()) {
              handleRequestsOnce();
          }
      }) {}

template <class CallbackType, class RequestType>
void FakeVehicleHardware::PendingRequestHandler<CallbackType, RequestType>::addRequest(
        RequestType request, std::shared_ptr<const CallbackType> callback) {
    mRequests.push({
            request,
            callback,
    });
}

template <class CallbackType, class RequestType>
void FakeVehicleHardware::PendingRequestHandler<CallbackType, RequestType>::stop() {
    mRequests.deactivate();
    if (mThread.joinable()) {
        mThread.join();
    }
}

template <>
void FakeVehicleHardware::PendingRequestHandler<FakeVehicleHardware::GetValuesCallback,
                                                GetValueRequest>::handleRequestsOnce() {
    std::unordered_map<std::shared_ptr<const GetValuesCallback>, std::vector<GetValueResult>>
            callbackToResults;
    for (const auto& rwc : mRequests.flush()) {
        auto result = mHardware->handleGetValueRequest(rwc.request);
        callbackToResults[rwc.callback].push_back(std::move(result));
    }
    for (const auto& [callback, results] : callbackToResults) {
        (*callback)(std::move(results));
    }
}

template <>
void FakeVehicleHardware::PendingRequestHandler<FakeVehicleHardware::SetValuesCallback,
                                                SetValueRequest>::handleRequestsOnce() {
    std::unordered_map<std::shared_ptr<const SetValuesCallback>, std::vector<SetValueResult>>
            callbackToResults;
    for (const auto& rwc : mRequests.flush()) {
        auto result = mHardware->handleSetValueRequest(rwc.request);
        callbackToResults[rwc.callback].push_back(std::move(result));
    }
    for (const auto& [callback, results] : callbackToResults) {
        (*callback)(std::move(results));
    }
}

}  // namespace fake
}  // namespace vehicle
}  // namespace automotive
}  // namespace hardware
}  // namespace android
