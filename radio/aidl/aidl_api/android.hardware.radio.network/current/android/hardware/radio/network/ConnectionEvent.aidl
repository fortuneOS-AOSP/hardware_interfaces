/*
 * Copyright 2023 The Android Open Source Project
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
///////////////////////////////////////////////////////////////////////////////
// THIS FILE IS IMMUTABLE. DO NOT EDIT IN ANY CASE.                          //
///////////////////////////////////////////////////////////////////////////////

// This file is a snapshot of an AIDL file. Do not edit it manually. There are
// two cases:
// 1). this is a frozen version file - do not edit this in any case.
// 2). this is a 'current' file. If you make a backwards compatible change to
//     the interface (from the latest frozen version), the build system will
//     prompt you to update this file with `m <name>-update-api`.
//
// You must not make a backward incompatible change to any AIDL file built
// with the aidl_interface module type with versions property set. The module
// type is used to build AIDL files in a way that they can be used across
// independently updatable components of the system. If a device is shipped
// with such a backward incompatible change, it has a high risk of breaking
// later when a module using the interface is updated, e.g., Mainline modules.

package android.hardware.radio.network;
/* @hide */
@Backing(type="int") @JavaDerive(toString=true) @VintfStability
enum ConnectionEvent {
  CS_SIGNALLING_GSM = 0,
  PS_SIGNALLING_GPRS = 1,
  CS_SIGNALLING_3G = 2,
  PS_SIGNALLING_3G = 3,
  NAS_SIGNALLING_LTE = 4,
  AS_SIGNALLING_LTE = 5,
  VOLTE_SIP = 6,
  VOLTE_SIP_SOS = 7,
  VOLTE_RTP = 8,
  VOLTE_RTP_SOS = 9,
  NAS_SIGNALLING_5G = 10,
  AS_SIGNALLING_5G = 11,
  VONR_SIP = 12,
  VONR_SIP_SOS = 13,
  VONR_RTP = 14,
  VONR_RTP_SOS = 15,
}
