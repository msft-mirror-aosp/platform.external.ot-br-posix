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

package com.android.server.openthread;

import com.android.server.openthread.Ipv6AddressInfo;

/** OpenThread daemon callbacks. */
oneway interface IOtDaemonCallback {
    /**
     * Called when the Thread interface state has been changed.
     *
     * @param isUp indicates whether the interface is up
     */
    void onInterfaceStateChanged(boolean isUp);

    /**
     * Called when the Thread device role has been changed.
     *
     * @param deviceRole the new Thread device role
     */
    void onDeviceRoleChanged(int deviceRole);

    /**
     * Called when the Thread network partition ID has been changed.
     *
     * @param partitionId the new Thread network partition ID
     */
    void onPartitionIdChanged(long partitionId);

    /**
     * Called when the Thread network Active Operational Dataset has been changed.
     *
     * @param activeOpDatasetTlvs the new Active Operational Dataset encoded as Thread TLV list. An
     *                            empty array indicates absence/lost of the Active Operational
     *                            Dataset
     */
    void onActiveOperationalDatasetChanged(in byte[] activeOpDatasetTlvs);

    /**
     * Called when the Thread network Pending Operational Dataset has been changed.
     *
     * @param pendingOpDatasetTlvs the new Pending Operational Dataset encoded as Thread TLV list.
     *                             An empty array indicates absence/lost of the Pending Operational
     *                             Dataset
     */
    void onPendingOperationalDatasetChanged(in byte[] pendingOpDatasetTlvs);

    /**
     * Called when Thread interface address has been changed.
     *
     * @param addressInfo the IPv6 address which has been updated. This can be both unicast and
     *                    multicast addresses
     * @param isAdded {@code true} if this address is being added to the Thread interface;
     *                Otherwise, this address is being removed
     */
    void onAddressChanged(in Ipv6AddressInfo addressInfo, boolean isAdded);
}
