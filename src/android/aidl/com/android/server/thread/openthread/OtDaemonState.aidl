/*
 *    Copyright (c) 2023, The OpenThread Authors.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *    3. Neither the name of the copyright holder nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *    POSSIBILITY OF SUCH DAMAGE.
 */

package com.android.server.thread.openthread;

/**
 * Contains all OpenThread daemon states which the system_server and/or client apps care about.
 */
parcelable OtDaemonState {
    boolean isInterfaceUp;

    // Valid values are DEVICE_ROLE_* defined in {@link ThreadNetworkController}.
    // Those are also OT_DEVICE_ROLE_* defined in external/openthread/include/openthread/thread.h
    // TODO: add unit tests to make sure those are equal to each other
    int deviceRole;

    long partitionId;

    // Active Oprational Dataset encoded as Thread TLVs. Empty array means the dataset doesn't
    // exist
    byte[] activeDatasetTlvs;

    // Active Oprational Dataset encoded as Thread TLVs. Empty array means the dataset doesn't
    // exist
    byte[] pendingDatasetTlvs;

    // The Thread enabled state OT_STATE_DISABLED, OT_STATE_ENABLED and OT_STATE_DISABLING.
    int threadEnabled;

    // The ephemeral key state EPHEMERAL_KEY_DISABLED, EPHEMERAL_KEY_ENABLED, EPHEMERAL_KEY_IN_USE
    // defined in {@link ThreadNetworkController}.
    int ephemeralKeyState;

    // The ephemeral key passcode string, valid when ephemeralKeyState is not
    // EPHEMERAL_KEY_DISABLED.
    String ephemeralKeyPasscode;

    // The ephemeral key lifetime in milliseconds, or 0 when ephemeralKeyState is
    // EPHEMERAL_KEY_DISABLED.
    long ephemeralKeyLifetimeMillis;
}
