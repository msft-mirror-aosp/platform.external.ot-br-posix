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

import static com.android.server.thread.openthread.IOtDaemon.ErrorCode.OT_ERROR_NOT_IMPLEMENTED;
import static com.android.server.thread.openthread.IOtDaemon.OT_EPHEMERAL_KEY_DISABLED;
import static com.android.server.thread.openthread.IOtDaemon.OT_EPHEMERAL_KEY_ENABLED;
import static com.android.server.thread.openthread.IOtDaemon.OT_STATE_DISABLED;
import static com.android.server.thread.openthread.IOtDaemon.OT_STATE_ENABLED;

import android.annotation.NonNull;
import android.annotation.Nullable;
import android.net.thread.ChannelMaxPower;
import android.os.Handler;
import android.os.IBinder;
import android.os.IBinder.DeathRecipient;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;

import com.android.server.thread.openthread.BackboneRouterState;
import com.android.server.thread.openthread.IChannelMasksReceiver;
import com.android.server.thread.openthread.INsdPublisher;
import com.android.server.thread.openthread.IOtDaemon;
import com.android.server.thread.openthread.IOtDaemonCallback;
import com.android.server.thread.openthread.IOtOutputReceiver;
import com.android.server.thread.openthread.IOtStatusReceiver;
import com.android.server.thread.openthread.MeshcopTxtAttributes;
import com.android.server.thread.openthread.OtDaemonConfiguration;
import com.android.server.thread.openthread.OtDaemonState;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
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
    static final int OT_ERROR_NONE = 0;

    private static final long PROACTIVE_LISTENER_ID = -1;

    private final Handler mHandler;
    private OtDaemonState mState;
    private BackboneRouterState mBbrState;
    private boolean mIsInitialized = false;
    private int mChannelMasksReceiverOtError = OT_ERROR_NONE;
    private int mSupportedChannelMask = 0x07FFF800; // from channel 11 to 26
    private int mPreferredChannelMask = 0;
    private boolean mTrelEnabled = false;

    @Nullable private DeathRecipient mDeathRecipient;
    @Nullable private ParcelFileDescriptor mTunFd;
    @Nullable private INsdPublisher mNsdPublisher;
    @Nullable private MeshcopTxtAttributes mOverriddenMeshcopTxts;
    @Nullable private IOtDaemonCallback mCallback;
    @Nullable private Long mCallbackListenerId;
    @Nullable private RemoteException mJoinException;
    @Nullable private String mNat64Cidr;
    @Nullable private RemoteException mSetNat64CidrException;
    @Nullable private RemoteException mRunOtCtlCommandException;
    @Nullable private String mCountryCode;
    @Nullable private OtDaemonConfiguration mConfiguration;

    public FakeOtDaemon(Handler handler) {
        mHandler = handler;
        resetStates();
    }

    private void resetStates() {
        mState = new OtDaemonState();
        mState.isInterfaceUp = false;
        mState.partitionId = -1;
        mState.deviceRole = OT_DEVICE_ROLE_DISABLED;
        mState.activeDatasetTlvs = new byte[0];
        mState.pendingDatasetTlvs = new byte[0];
        mState.threadEnabled = OT_STATE_DISABLED;
        mState.ephemeralKeyState = OT_EPHEMERAL_KEY_DISABLED;
        mState.ephemeralKeyPasscode = "";
        mState.ephemeralKeyLifetimeMillis = 0;
        mBbrState = new BackboneRouterState();
        mBbrState.multicastForwardingEnabled = false;
        mBbrState.listeningAddresses = new ArrayList<>();
        mConfiguration = null;

        mTunFd = null;
        mNsdPublisher = null;
        mIsInitialized = false;

        mCallback = null;
        mCallbackListenerId = null;
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

    @Nullable
    public DeathRecipient getDeathRecipient() {
        return mDeathRecipient;
    }

    @Override
    public void initialize(
            boolean enabled,
            OtDaemonConfiguration config,
            ParcelFileDescriptor tunFd,
            INsdPublisher nsdPublisher,
            MeshcopTxtAttributes overriddenMeshcopTxts,
            String countryCode,
            boolean trelEnabled,
            IOtDaemonCallback callback)
            throws RemoteException {
        mIsInitialized = true;

        mState.threadEnabled = enabled ? OT_STATE_ENABLED : OT_STATE_DISABLED;
        setConfiguration(config, null /* receiver */);
        mTunFd = tunFd;
        mNsdPublisher = nsdPublisher;
        mCountryCode = countryCode;
        mTrelEnabled = trelEnabled;

        mOverriddenMeshcopTxts = new MeshcopTxtAttributes();
        mOverriddenMeshcopTxts.vendorOui = overriddenMeshcopTxts.vendorOui.clone();
        mOverriddenMeshcopTxts.vendorName = overriddenMeshcopTxts.vendorName;
        mOverriddenMeshcopTxts.modelName = overriddenMeshcopTxts.modelName;
        mOverriddenMeshcopTxts.nonStandardTxtEntries =
                List.copyOf(overriddenMeshcopTxts.nonStandardTxtEntries);

        registerStateCallback(callback, PROACTIVE_LISTENER_ID);
    }

    /** Returns {@code true} if {@link initialize} has been called to initialize this object. */
    public boolean isInitialized() {
        return mIsInitialized;
    }

    @Override
    public void terminate() throws RemoteException {
        mHandler.post(
                () -> {
                    resetStates();
                    if (mDeathRecipient != null) {
                        mDeathRecipient.binderDied();
                        mDeathRecipient = null;
                    }
                });
    }

    public int getEnabledState() {
        return mState.threadEnabled;
    }

    public OtDaemonState getState() {
        return makeCopy(mState);
    }

    public BackboneRouterState getBackboneRouterState() {
        return makeCopy(mBbrState);
    }

    /**
     * Returns the Thread TUN interface FD sent to OT daemon or {@code null} if {@link initialize}
     * is never called.
     */
    @Nullable
    public ParcelFileDescriptor getTunFd() {
        return mTunFd;
    }

    /**
     * Returns the INsdPublisher sent to OT daemon or {@code null} if {@link #initialize} is never
     * called.
     */
    @Nullable
    public INsdPublisher getNsdPublisher() {
        return mNsdPublisher;
    }

    /**
     * Returns the overridden MeshCoP TXT attributes that is to OT daemon or {@code null} if {@link
     * #initialize} is never called.
     */
    @Nullable
    public MeshcopTxtAttributes getOverriddenMeshcopTxtAttributes() {
        return mOverriddenMeshcopTxts;
    }

    @Nullable
    public IOtDaemonCallback getCallback() {
        return mCallback;
    }

    @Override
    public void setThreadEnabled(boolean enabled, IOtStatusReceiver receiver) {
        mHandler.post(
                () -> {
                    mState.threadEnabled = enabled ? OT_STATE_ENABLED : OT_STATE_DISABLED;
                    try {
                        receiver.onSuccess();
                    } catch (RemoteException e) {
                        throw new AssertionError(e);
                    }
                });
    }

    @Override
    public void registerStateCallback(IOtDaemonCallback callback, long listenerId)
            throws RemoteException {
        mCallback = callback;
        mCallbackListenerId = listenerId;

        mHandler.post(() -> onStateChanged(mState, mCallbackListenerId));
        mHandler.post(() -> onBackboneRouterStateChanged(mBbrState));
    }

    @Nullable
    public IOtDaemonCallback getStateCallback() {
        return mCallback;
    }

    /**
     * Returns the country code sent to OT daemon or {@code null} if {@link #initialize} is never
     * called.
     */
    @Nullable
    public String getCountryCode() {
        return mCountryCode;
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
                    mBbrState.multicastForwardingEnabled = true;

                    onStateChanged(mState, PROACTIVE_LISTENER_ID);
                    onBackboneRouterStateChanged(mBbrState);
                    try {
                        receiver.onSuccess();
                    } catch (RemoteException e) {
                        throw new AssertionError(e);
                    }
                },
                JOIN_DELAY.toMillis());
    }

    @Override
    public void activateEphemeralKeyMode(long lifetimeMillis, IOtStatusReceiver receiver) {
        mHandler.post(
                () -> {
                    mState.ephemeralKeyState = OT_EPHEMERAL_KEY_ENABLED;
                    mState.ephemeralKeyPasscode = "123456789";
                    mState.ephemeralKeyLifetimeMillis = lifetimeMillis;
                    try {
                        receiver.onSuccess();
                    } catch (RemoteException e) {
                        throw new AssertionError(e);
                    }
                });
    }

    @Override
    public void deactivateEphemeralKeyMode(IOtStatusReceiver receiver) {
        mHandler.post(
                () -> {
                    mState.ephemeralKeyState = OT_EPHEMERAL_KEY_DISABLED;
                    mState.ephemeralKeyPasscode = "";
                    mState.ephemeralKeyLifetimeMillis = 0;
                    try {
                        receiver.onSuccess();
                    } catch (RemoteException e) {
                        throw new AssertionError(e);
                    }
                });
    }

    private OtDaemonState makeCopy(OtDaemonState state) {
        OtDaemonState copyState = new OtDaemonState();
        copyState.isInterfaceUp = state.isInterfaceUp;
        copyState.deviceRole = state.deviceRole;
        copyState.partitionId = state.partitionId;
        copyState.activeDatasetTlvs = state.activeDatasetTlvs.clone();
        copyState.pendingDatasetTlvs = state.pendingDatasetTlvs.clone();
        return copyState;
    }

    private BackboneRouterState makeCopy(BackboneRouterState state) {
        BackboneRouterState copyState = new BackboneRouterState();
        copyState.multicastForwardingEnabled = state.multicastForwardingEnabled;
        copyState.listeningAddresses = new ArrayList<>(state.listeningAddresses);
        return copyState;
    }

    private void onStateChanged(OtDaemonState state, long listenerId) {
        try {
            // Make a copy of state so that clients won't keep a direct reference to it
            OtDaemonState copyState = makeCopy(state);

            mCallback.onStateChanged(copyState, listenerId);
        } catch (RemoteException e) {
            throw new AssertionError(e);
        }
    }

    private void onBackboneRouterStateChanged(BackboneRouterState state) {
        try {
            // Make a copy of state so that clients won't keep a direct reference to it
            BackboneRouterState copyState = makeCopy(state);

            mCallback.onBackboneRouterStateChanged(copyState);
        } catch (RemoteException e) {
            throw new AssertionError(e);
        }
    }

    /** Sets the {@link RemoteException} which will be thrown from {@link #join}. */
    public void setJoinException(RemoteException exception) {
        mJoinException = exception;
    }

    @Override
    public void leave(boolean eraseDataset, IOtStatusReceiver receiver) throws RemoteException {
        throw new UnsupportedOperationException("FakeOtDaemon#leave is not implemented!");
    }

    @Override
    public void setConfiguration(OtDaemonConfiguration config, IOtStatusReceiver receiver)
            throws RemoteException {
        mConfiguration = config;
        // TODO: b/343814054 - Support enabling/disabling DHCPv6-PD.
        if (mConfiguration.dhcpv6PdEnabled) {
            receiver.onError(OT_ERROR_NOT_IMPLEMENTED, "DHCPv6-PD is not supported");
            return;
        }
        if (receiver != null) {
            receiver.onSuccess();
        }
    }

    /** Returns the configuration set by {@link #initialize} or {@link #setConfiguration}. */
    @Nullable
    public OtDaemonConfiguration getConfiguration() {
        return mConfiguration;
    }

    @Override
    public void setInfraLinkInterfaceName(
            String interfaceName, ParcelFileDescriptor fd, IOtStatusReceiver receiver)
            throws RemoteException {
        throw new UnsupportedOperationException(
                "FakeOtDaemon#setInfraLinkInterfaceName is not implemented!");
    }

    @Override
    public void setInfraLinkNat64Prefix(String nat64Prefix, IOtStatusReceiver receiver)
            throws RemoteException {
        throw new UnsupportedOperationException(
                "FakeOtDaemon#setInfraLinkNat64Prefix is not implemented!");
    }

    /** Sets the {@link RemoteException} which will be thrown from {@link #setNat64Cidr}. */
    public void setSetNat64CidrException(RemoteException exception) {
        mSetNat64CidrException = exception;
    }

    @Override
    public void setNat64Cidr(String nat64Cidr, IOtStatusReceiver receiver) throws RemoteException {
        if (mSetNat64CidrException != null) {
            throw mSetNat64CidrException;
        }
        mNat64Cidr = nat64Cidr;
        if (receiver != null) {
            receiver.onSuccess();
        }
    }

    /** Returns the NAT64 CIDR set by {@link #setNat64Cidr}. */
    @Nullable
    public String getNat64Cidr() {
        return mNat64Cidr;
    }

    @Override
    public void setInfraLinkDnsServers(List<String> dnsServers, IOtStatusReceiver receiver)
            throws RemoteException {
        throw new UnsupportedOperationException(
                "FakeOtDaemon#setInfraLinkDnsServers is not implemented!");
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
        throw new UnsupportedOperationException("FakeOtDaemon#setCountryCode is not implemented!");
    }

    @Override
    public void getChannelMasks(IChannelMasksReceiver receiver) throws RemoteException {
        mHandler.post(
                () -> {
                    try {
                        if (mChannelMasksReceiverOtError == OT_ERROR_NONE) {
                            receiver.onSuccess(mSupportedChannelMask, mPreferredChannelMask);
                        } else {
                            receiver.onError(
                                    mChannelMasksReceiverOtError, "Get channel masks failed");
                        }
                    } catch (RemoteException e) {
                        throw new AssertionError(e);
                    }
                });
    }

    public void setChannelMasks(int supportedChannelMask, int preferredChannelMask) {
        mSupportedChannelMask = supportedChannelMask;
        mPreferredChannelMask = preferredChannelMask;
    }

    public void setChannelMasksReceiverOtError(int otError) {
        mChannelMasksReceiverOtError = otError;
    }

    @Override
    public void setChannelMaxPowers(ChannelMaxPower[] channelMaxPowers, IOtStatusReceiver receiver)
            throws RemoteException {
        throw new UnsupportedOperationException(
                "FakeOtDaemon#setChannelTargetPowers is not implemented!");
    }

    @Override
    public void runOtCtlCommand(String command, boolean isInteractive, IOtOutputReceiver receiver)
            throws RemoteException {
        if (mRunOtCtlCommandException != null) {
            throw mRunOtCtlCommandException;
        }

        mHandler.post(
                () -> {
                    try {
                        List<String> outputLines = new ArrayList<>();
                        outputLines.add("leader");
                        outputLines.add("\r\n");
                        outputLines.add("Done");
                        outputLines.add("\r\n");

                        for (String line : outputLines) {
                            receiver.onOutput(line);
                        }
                        receiver.onComplete();
                    } catch (RemoteException e) {
                        throw new AssertionError(e);
                    }
                });
    }

    /** Sets the {@link RemoteException} which will be thrown from {@link #runOtCtlCommand}. */
    public void setRunOtCtlCommandException(RemoteException exception) {
        mRunOtCtlCommandException = exception;
    }

    public boolean isTrelEnabled() {
        return mTrelEnabled;
    }
}
