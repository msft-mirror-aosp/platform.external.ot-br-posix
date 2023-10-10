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

import android.os.ParcelFileDescriptor;

import com.android.server.thread.openthread.Ipv6AddressInfo;
import com.android.server.thread.openthread.IOtStatusReceiver;
import com.android.server.thread.openthread.IOtDaemonCallback;

/**
 * The OpenThread daemon service which provides access to the core Thread stack for
 * system_server.
 */
interface IOtDaemon {
    /**
     * The Thread tunnel interface name. This interface MUST be created before
     * starting this {@link IOtDaemon} service.
     */
    const String TUN_IF_NAME = "thread-wpan";

    /**
     * Initializes this service with Thread tunnel interface FD and stack callback.
     *
     * @param tunFd the Thread tunnel interface FD which can be used to transmit/receive
     *              packets to/from Thread PAN
     * @param callback the cllback for receiving all Thread stack events
     */
    // Okay to be blocking API, this doesn't call into OT stack
    void initialize(in ParcelFileDescriptor tunFd, in IOtDaemonCallback callback);

    /** Returns the Extended MAC Address of this Thread device. */
    // Okay to be blocking, this is already cached in memory
    byte[] getExtendedMacAddress();

    /** Returns the Thread version that this Thread device is running. */
    // Okay to be blocking, this is in-memory-only value
    int getThreadVersion();

    /**
     * Attaches this device to the network specified by {@code activeOpDatasetTlvs}.
     *
     * @sa android.net.thread.ThreadNetworkController#attach
     * @sa android.net.thread.ThreadNetworkController#attachOrForm
     */
    oneway void attach(
        boolean doForm, in byte[] activeOpDatasetTlvs, in IOtStatusReceiver receiver);

    /**
     * Detaches from the current network.
     *
     * 1. It returns success immediately if this device is already detached or disabled
     * 2. Else if there is already an onging {@code detach} request, no action will be taken but
     *    the {@code receiver} will be invoked after the previous request is completed
     * 3. Otherwise, OTBR sends Address Release Notification (i.e. ADDR_REL.ntf) to grcefully
     *    detach from the current network and it takes 1 second to finish
     *
     * @sa android.net.thread.ThreadNetworkController#detach
     */
    oneway void detach(in IOtStatusReceiver receiver);

    /** Migrates to the new network specified by {@code pendingOpDatasetTlvs}.
     *
     * @sa android.net.thread.ThreadNetworkController#scheduleMigration
     */
    oneway void scheduleMigration(
        in byte[] pendingOpDatasetTlvs, in IOtStatusReceiver receiver);

    // TODO: add Border Router APIs
}
