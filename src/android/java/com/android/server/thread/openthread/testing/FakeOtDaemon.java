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

package com.android.server.thread.openthread.testing;

import android.annotation.NonNull;
import android.annotation.Nullable;
import android.os.Handler;
import android.os.IBinder;
import android.os.IBinder.DeathRecipient;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;

import com.android.server.thread.openthread.BorderRouterConfigurationParcel;
import com.android.server.thread.openthread.IOtDaemon;
import com.android.server.thread.openthread.IOtDaemonCallback;
import com.android.server.thread.openthread.IOtStatusReceiver;
import com.android.server.thread.openthread.OtDaemonState;

import java.time.Duration;
import java.util.NoSuchElementException;

/** A fake implementation of the {@link IOtDaemon} AIDL API for testing. */
public final class FakeOtDaemon extends IOtDaemon.Stub {
    /** The typical Thread network join / attach delay is around 8 seconds. */
    public static final Duration JOIN_DELAY = Duration.ofSeconds(8);

    static final int OT_DEVICE_ROLE_DISABLED = 0;
    static final int OT_DEVICE_ROLE_DETACHED = 1;
    static final int OT_DEVICE_ROLE_CHILD = 2;
    static final int OT_DEVICE_ROLE_ROUTER = 3;
    static final int OT_DEVICE_ROLE_LEADER = 4;

    private static final long PROACTIVE_LISTENER_ID = -1;

    private final Handler mHandler;
    private final OtDaemonState mState;

    @Nullable private DeathRecipient mDeathRecipient;

    @Nullable private ParcelFileDescriptor mTunFd;

    @Nullable private IOtDaemonCallback mCallback;

    @Nullable private Long mCallbackListenerId;

    @Nullable private RemoteException mJoinException;

    public FakeOtDaemon(Handler handler) {
        mHandler = handler;
        mState = new OtDaemonState();
        mState.isInterfaceUp = false;
        mState.deviceRole = OT_DEVICE_ROLE_DISABLED;
        mState.activeDatasetTlvs = new byte[0];
        mState.pendingDatasetTlvs = new byte[0];
        mState.multicastForwardingEnabled = false;
    }

    @Override
    public IBinder asBinder() {
        return this;
    }

    @Override
    public void linkToDeath(DeathRecipient recipient, int flags) {
        if (mDeathRecipient != null && recipient != null) {
            throw new IllegalStateException("IOtDaemon death recipient is already linked!");
        }

        mDeathRecipient = recipient;
    }

    @Override
    public boolean unlinkToDeath(@NonNull DeathRecipient recipient, int flags) {
        if (mDeathRecipient == null || recipient != mDeathRecipient) {
            throw new NoSuchElementException("recipient is not linked! " + recipient);
        }

        mDeathRecipient = null;
        return true;
    }

    @Override
    public void initialize(ParcelFileDescriptor tunFd) throws RemoteException {
        mTunFd = tunFd;
    }

    /**
     * Returns the Thread TUN interface FD sent to OT daemon or {@code null} if {@link initialize}
     * is never called.
     */
    @Nullable
    public ParcelFileDescriptor getTunFd() {
        return mTunFd;
    }

    @Override
    public void registerStateCallback(IOtDaemonCallback callback, long listenerId)
            throws RemoteException {
        mCallback = callback;
        mCallbackListenerId = listenerId;

        mHandler.post(() -> onStateChanged(mState, mCallbackListenerId));
    }

    @Nullable
    public IOtDaemonCallback getStateCallback() {
        return mCallback;
    }

    @Override
    public void join(byte[] activeDataset, IOtStatusReceiver receiver) throws RemoteException {
        if (mJoinException != null) {
            throw mJoinException;
        }

        mHandler.post(
                () -> {
                    mState.isInterfaceUp = true;
                    mState.deviceRole = OT_DEVICE_ROLE_DETACHED;
                    onStateChanged(mState, PROACTIVE_LISTENER_ID);
                });

        mHandler.postDelayed(
                () -> {
                    mState.deviceRole = OT_DEVICE_ROLE_LEADER;
                    mState.activeDatasetTlvs = activeDataset.clone();
                    mState.multicastForwardingEnabled = true;

                    onStateChanged(mState, PROACTIVE_LISTENER_ID);
                    try {
                        receiver.onSuccess();
                    } catch (RemoteException e) {
                        throw new AssertionError(e);
                    }
                },
                JOIN_DELAY.toMillis());
    }

    private void onStateChanged(OtDaemonState state, long listenerId) {
        try {
            // Make a copy of mState so that clients won't keep a direct reference to it
            OtDaemonState copyState = new OtDaemonState();
            copyState.isInterfaceUp = state.isInterfaceUp;
            copyState.deviceRole = state.deviceRole;
            copyState.partitionId = state.partitionId;
            copyState.activeDatasetTlvs = state.activeDatasetTlvs.clone();
            copyState.pendingDatasetTlvs = state.pendingDatasetTlvs.clone();
            copyState.multicastForwardingEnabled = state.multicastForwardingEnabled;

            mCallback.onStateChanged(copyState, listenerId);
        } catch (RemoteException e) {
            throw new AssertionError(e);
        }
    }

    /** Sets the {@link RemoteException} which will be thrown from {@link #join}. */
    public void setJoinException(RemoteException exception) {
        mJoinException = exception;
    }

    @Override
    public void leave(IOtStatusReceiver receiver) throws RemoteException {
        throw new UnsupportedOperationException("FakeOtDaemon#leave is not implemented!");
    }

    @Override
    public void configureBorderRouter(
            BorderRouterConfigurationParcel config, IOtStatusReceiver receiver)
            throws RemoteException {
        throw new UnsupportedOperationException(
                "FakeOtDaemon#configureBorderRouter is not implemented!");
    }

    @Override
    public void scheduleMigration(byte[] pendingDataset, IOtStatusReceiver receiver)
            throws RemoteException {
        throw new UnsupportedOperationException(
                "FakeOtDaemon#scheduleMigration is not implemented!");
    }

    @Override
    public void setCountryCode(String countryCode, IOtStatusReceiver receiver)
            throws RemoteException {
        throw new UnsupportedOperationException(
                "FakeOtDaemon#scheduleMigration is not implemented!");
    }
}
