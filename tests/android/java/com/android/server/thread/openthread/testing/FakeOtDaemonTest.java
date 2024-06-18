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

import static com.android.server.thread.openthread.IOtDaemon.ErrorCode.OT_ERROR_INVALID_STATE;
import static com.android.server.thread.openthread.IOtDaemon.OT_STATE_DISABLED;
import static com.android.server.thread.openthread.IOtDaemon.OT_STATE_ENABLED;
import static com.android.server.thread.openthread.testing.FakeOtDaemon.OT_DEVICE_ROLE_DISABLED;

import static com.google.common.io.BaseEncoding.base16;
import static com.google.common.truth.Truth.assertThat;

import static org.junit.Assert.assertThrows;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.anyLong;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;

import android.os.Handler;
import android.os.IBinder.DeathRecipient;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.test.TestLooper;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.filters.SmallTest;

import com.android.server.thread.openthread.BackboneRouterState;
import com.android.server.thread.openthread.DnsTxtAttribute;
import com.android.server.thread.openthread.IChannelMasksReceiver;
import com.android.server.thread.openthread.INsdPublisher;
import com.android.server.thread.openthread.IOtDaemonCallback;
import com.android.server.thread.openthread.IOtStatusReceiver;
import com.android.server.thread.openthread.MeshcopTxtAttributes;
import com.android.server.thread.openthread.OtDaemonState;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.MockitoAnnotations;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/** Unit tests for {@link FakeOtDaemon}. */
@SmallTest
@RunWith(AndroidJUnit4.class)
public final class FakeOtDaemonTest {
    // A valid Thread Active Operational Dataset generated from OpenThread CLI "dataset new":
    // Active Timestamp: 1
    // Channel: 19
    // Channel Mask: 0x07FFF800
    // Ext PAN ID: ACC214689BC40BDF
    // Mesh Local Prefix: fd64:db12:25f4:7e0b::/64
    // Network Key: F26B3153760F519A63BAFDDFFC80D2AF
    // Network Name: OpenThread-d9a0
    // PAN ID: 0xD9A0
    // PSKc: A245479C836D551B9CA557F7B9D351B4
    // Security Policy: 672 onrcb
    private static final byte[] DEFAULT_ACTIVE_DATASET_TLVS =
            base16().decode(
                            "0E080000000000010000000300001335060004001FFFE002"
                                    + "08ACC214689BC40BDF0708FD64DB1225F47E0B0510F26B31"
                                    + "53760F519A63BAFDDFFC80D2AF030F4F70656E5468726561"
                                    + "642D643961300102D9A00410A245479C836D551B9CA557F7"
                                    + "B9D351B40C0402A0FFF8");

    private static final int DEFAULT_SUPPORTED_CHANNEL_MASK = 0x07FFF800; // from channel 11 to 26
    private static final int DEFAULT_PREFERRED_CHANNEL_MASK = 0;
    private static final byte[] TEST_VENDOR_OUI = new byte[] {(byte) 0xAC, (byte) 0xDE, 0x48};
    private static final String TEST_VENDOR_NAME = "test vendor";
    private static final String TEST_MODEL_NAME = "test model";
    private static final String TEST_DEFAULT_COUNTRY_CODE = "WW";

    private FakeOtDaemon mFakeOtDaemon;
    private TestLooper mTestLooper;
    @Mock private ParcelFileDescriptor mMockTunFd;
    @Mock private INsdPublisher mMockNsdPublisher;
    @Mock private IOtDaemonCallback mMockCallback;
    private MeshcopTxtAttributes mOverriddenMeshcopTxts;

    @Before
    public void setUp() {
        MockitoAnnotations.initMocks(this);

        mTestLooper = new TestLooper();
        mFakeOtDaemon = new FakeOtDaemon(new Handler(mTestLooper.getLooper()));
        mOverriddenMeshcopTxts = new MeshcopTxtAttributes();
        mOverriddenMeshcopTxts.vendorName = TEST_VENDOR_NAME;
        mOverriddenMeshcopTxts.vendorOui = TEST_VENDOR_OUI;
        mOverriddenMeshcopTxts.modelName = TEST_MODEL_NAME;
        mOverriddenMeshcopTxts.nonStandardTxtEntries = List.of();
    }

    @Test
    public void initialize_succeed_argumentsAreSetAndCallbackIsInvoked() throws Exception {
        mOverriddenMeshcopTxts.vendorName = TEST_VENDOR_NAME;
        mOverriddenMeshcopTxts.vendorOui = TEST_VENDOR_OUI;
        mOverriddenMeshcopTxts.modelName = TEST_MODEL_NAME;
        mOverriddenMeshcopTxts.nonStandardTxtEntries =
                List.of(new DnsTxtAttribute("v2", new byte[] {0x02}));

        mFakeOtDaemon.initialize(
                mMockTunFd,
                true,
                mMockNsdPublisher,
                mOverriddenMeshcopTxts,
                mMockCallback,
                TEST_DEFAULT_COUNTRY_CODE);
        mTestLooper.dispatchAll();

        MeshcopTxtAttributes meshcopTxts = mFakeOtDaemon.getOverriddenMeshcopTxtAttributes();
        assertThat(meshcopTxts).isNotNull();
        assertThat(meshcopTxts.vendorName).isEqualTo(TEST_VENDOR_NAME);
        assertThat(meshcopTxts.vendorOui).isEqualTo(TEST_VENDOR_OUI);
        assertThat(meshcopTxts.modelName).isEqualTo(TEST_MODEL_NAME);
        assertThat(meshcopTxts.nonStandardTxtEntries)
                .containsExactly(new DnsTxtAttribute("v2", new byte[] {0x02}));
        assertThat(mFakeOtDaemon.getTunFd()).isEqualTo(mMockTunFd);
        assertThat(mFakeOtDaemon.getEnabledState()).isEqualTo(OT_STATE_ENABLED);
        assertThat(mFakeOtDaemon.getNsdPublisher()).isEqualTo(mMockNsdPublisher);
        assertThat(mFakeOtDaemon.getStateCallback()).isEqualTo(mMockCallback);
        assertThat(mFakeOtDaemon.getCountryCode()).isEqualTo(TEST_DEFAULT_COUNTRY_CODE);
        assertThat(mFakeOtDaemon.isInitialized()).isTrue();
        verify(mMockCallback, times(1)).onStateChanged(any(), anyLong());
        verify(mMockCallback, times(1)).onBackboneRouterStateChanged(any());
    }

    @Test
    public void registerStateCallback_noStateChange_callbackIsInvoked() throws Exception {
        mFakeOtDaemon.initialize(
                mMockTunFd,
                true,
                mMockNsdPublisher,
                mOverriddenMeshcopTxts,
                mMockCallback,
                TEST_DEFAULT_COUNTRY_CODE);
        final AtomicReference<OtDaemonState> stateRef = new AtomicReference<>();
        final AtomicLong listenerIdRef = new AtomicLong();
        final AtomicReference<BackboneRouterState> bbrStateRef = new AtomicReference<>();

        mFakeOtDaemon.registerStateCallback(
                new IOtDaemonCallback.Default() {
                    @Override
                    public void onStateChanged(OtDaemonState newState, long listenerId) {
                        stateRef.set(newState);
                        listenerIdRef.set(listenerId);
                    }

                    @Override
                    public void onBackboneRouterStateChanged(BackboneRouterState bbrState) {
                        bbrStateRef.set(bbrState);
                    }
                },
                7 /* listenerId */);
        mTestLooper.dispatchAll();

        OtDaemonState state = stateRef.get();
        assertThat(state).isNotNull();
        assertThat(state.isInterfaceUp).isFalse();
        assertThat(state.deviceRole).isEqualTo(FakeOtDaemon.OT_DEVICE_ROLE_DISABLED);
        assertThat(state.activeDatasetTlvs).isEmpty();
        assertThat(state.pendingDatasetTlvs).isEmpty();
        assertThat(state.threadEnabled).isEqualTo(OT_STATE_DISABLED);
        assertThat(listenerIdRef.get()).isEqualTo(7);
        BackboneRouterState bbrState = bbrStateRef.get();
        assertThat(bbrState.multicastForwardingEnabled).isFalse();
    }

    @Test
    public void setJoinException_joinFailsWithTheGivenException() {
        final RemoteException joinException = new RemoteException("join() failed");

        mFakeOtDaemon.setJoinException(joinException);

        RemoteException thrown =
                assertThrows(
                        RemoteException.class,
                        () ->
                                mFakeOtDaemon.join(
                                        DEFAULT_ACTIVE_DATASET_TLVS,
                                        new IOtStatusReceiver.Default()));
        assertThat(thrown).isEqualTo(joinException);
    }

    @Test
    public void join_succeed_statesAreSentBack() throws Exception {
        final AtomicBoolean succeedRef = new AtomicBoolean(false);
        final AtomicReference<OtDaemonState> stateRef = new AtomicReference<>();
        final AtomicReference<BackboneRouterState> bbrStateRef = new AtomicReference<>();
        mFakeOtDaemon.registerStateCallback(
                new IOtDaemonCallback.Default() {
                    @Override
                    public void onStateChanged(OtDaemonState newState, long listenerId) {
                        stateRef.set(newState);
                    }

                    @Override
                    public void onBackboneRouterStateChanged(BackboneRouterState bbrState) {
                        bbrStateRef.set(bbrState);
                    }
                },
                11 /* listenerId */);

        mFakeOtDaemon.join(
                DEFAULT_ACTIVE_DATASET_TLVS,
                new IOtStatusReceiver.Default() {
                    @Override
                    public void onSuccess() {
                        succeedRef.set(true);
                    }
                });
        mTestLooper.dispatchAll();
        final OtDaemonState intermediateDetachedState = stateRef.get();
        mTestLooper.moveTimeForward(FakeOtDaemon.JOIN_DELAY.toMillis());
        mTestLooper.dispatchAll();

        assertThat(intermediateDetachedState.isInterfaceUp).isTrue();
        assertThat(intermediateDetachedState.deviceRole)
                .isEqualTo(FakeOtDaemon.OT_DEVICE_ROLE_DETACHED);
        assertThat(succeedRef.get()).isTrue();
        final OtDaemonState state = stateRef.get();
        assertThat(state.isInterfaceUp).isTrue();
        assertThat(state.deviceRole).isEqualTo(FakeOtDaemon.OT_DEVICE_ROLE_LEADER);
        assertThat(state.activeDatasetTlvs).isEqualTo(DEFAULT_ACTIVE_DATASET_TLVS);
        final BackboneRouterState bbrState = bbrStateRef.get();
        assertThat(bbrState.multicastForwardingEnabled).isTrue();
    }

    @Test
    public void setThreadEnabled_enableThread_succeed() throws Exception {
        assertThat(mFakeOtDaemon.getEnabledState()).isEqualTo(OT_STATE_DISABLED);

        final AtomicBoolean succeedRef = new AtomicBoolean(false);
        mFakeOtDaemon.setThreadEnabled(
                true,
                new IOtStatusReceiver.Default() {
                    @Override
                    public void onSuccess() {
                        succeedRef.set(true);
                    }
                });
        mTestLooper.dispatchAll();

        assertThat(succeedRef.get()).isTrue();
        assertThat(mFakeOtDaemon.getEnabledState()).isEqualTo(OT_STATE_ENABLED);
    }

    @Test
    public void getChannelMasks_succeed_onSuccessIsInvoked() throws Exception {
        final AtomicInteger supportedChannelMaskRef = new AtomicInteger();
        final AtomicInteger preferredChannelMaskRef = new AtomicInteger();
        final AtomicBoolean errorRef = new AtomicBoolean(false);
        mFakeOtDaemon.setChannelMasks(
                DEFAULT_SUPPORTED_CHANNEL_MASK, DEFAULT_PREFERRED_CHANNEL_MASK);
        mFakeOtDaemon.setChannelMasksReceiverOtError(FakeOtDaemon.OT_ERROR_NONE);

        mFakeOtDaemon.getChannelMasks(
                new IChannelMasksReceiver.Default() {
                    @Override
                    public void onSuccess(int supportedChannelMask, int preferredChannelMask) {
                        supportedChannelMaskRef.set(supportedChannelMask);
                        preferredChannelMaskRef.set(preferredChannelMask);
                    }

                    @Override
                    public void onError(int otError, String message) {
                        errorRef.set(true);
                    }
                });
        mTestLooper.dispatchAll();

        assertThat(errorRef.get()).isFalse();
        assertThat(supportedChannelMaskRef.get()).isEqualTo(DEFAULT_SUPPORTED_CHANNEL_MASK);
        assertThat(preferredChannelMaskRef.get()).isEqualTo(DEFAULT_PREFERRED_CHANNEL_MASK);
    }

    @Test
    public void getChannelMasks_failed_onErrorIsInvoked() throws Exception {
        final AtomicInteger errorRef = new AtomicInteger(FakeOtDaemon.OT_ERROR_NONE);
        final AtomicBoolean succeedRef = new AtomicBoolean(false);
        mFakeOtDaemon.setChannelMasksReceiverOtError(OT_ERROR_INVALID_STATE);

        mFakeOtDaemon.getChannelMasks(
                new IChannelMasksReceiver.Default() {
                    @Override
                    public void onSuccess(int supportedChannelMask, int preferredChannelMask) {
                        succeedRef.set(true);
                    }

                    @Override
                    public void onError(int otError, String message) {
                        errorRef.set(otError);
                    }
                });
        mTestLooper.dispatchAll();

        assertThat(succeedRef.get()).isFalse();
        assertThat(errorRef.get()).isEqualTo(OT_ERROR_INVALID_STATE);
    }

    @Test
    public void terminate_statesAreResetAndDeathCallbackIsInvoked() throws Exception {
        DeathRecipient mockDeathRecipient = mock(DeathRecipient.class);
        mFakeOtDaemon.linkToDeath(mockDeathRecipient, 0);
        mFakeOtDaemon.initialize(
                mMockTunFd,
                true,
                mMockNsdPublisher,
                mOverriddenMeshcopTxts,
                mMockCallback,
                TEST_DEFAULT_COUNTRY_CODE);

        mFakeOtDaemon.terminate();
        mTestLooper.dispatchAll();

        assertThat(mFakeOtDaemon.isInitialized()).isFalse();
        OtDaemonState state = mFakeOtDaemon.getState();
        assertThat(state.isInterfaceUp).isEqualTo(false);
        assertThat(state.partitionId).isEqualTo(-1);
        assertThat(state.deviceRole).isEqualTo(OT_DEVICE_ROLE_DISABLED);
        assertThat(state.activeDatasetTlvs).isEqualTo(new byte[0]);
        assertThat(state.pendingDatasetTlvs).isEqualTo(new byte[0]);
        assertThat(state.threadEnabled).isEqualTo(OT_STATE_DISABLED);
        BackboneRouterState bbrState = mFakeOtDaemon.getBackboneRouterState();
        assertThat(bbrState.multicastForwardingEnabled).isFalse();
        assertThat(bbrState.listeningAddresses).isEqualTo(new ArrayList<>());
        assertThat(mFakeOtDaemon.getDeathRecipient()).isNull();
        assertThat(mFakeOtDaemon.getTunFd()).isNull();
        assertThat(mFakeOtDaemon.getNsdPublisher()).isNull();
        assertThat(mFakeOtDaemon.getEnabledState()).isEqualTo(OT_STATE_DISABLED);
        verify(mockDeathRecipient, times(1)).binderDied();
    }
}
