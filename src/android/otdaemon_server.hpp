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

#ifndef OTBR_ANDROID_BINDER_SERVER_HPP_
#define OTBR_ANDROID_BINDER_SERVER_HPP_

#include <functional>
#include <memory>
#include <vector>

#include <openthread/instance.h>
#include <openthread/ip6.h>

#include "common_utils.hpp"
#include "agent/vendor.hpp"
#include "android/android_thread_host.hpp"
#include "android/common_utils.hpp"
#include "android/mdns_publisher.hpp"
#include "common/mainloop.hpp"
#include "common/time.hpp"
#include "host/rcp_host.hpp"
#include "sdp_proxy/advertising_proxy.hpp"

namespace otbr {
namespace Android {

class OtDaemonServer : public BnOtDaemon, public MainloopProcessor, public vendor::VendorServer
{
public:
    using ResetThreadHandler = std::function<void()>;

    OtDaemonServer(otbr::Host::RcpHost    &aRcpHost,
                   otbr::Mdns::Publisher  &aMdnsPublisher,
                   otbr::BorderAgent      &aBorderAgent,
                   otbr::AdvertisingProxy &aAdvProxy,
                   ResetThreadHandler      aResetThreadHandler);
    virtual ~OtDaemonServer(void) = default;

    // Disallow copy and assign.
    OtDaemonServer(const OtDaemonServer &) = delete;
    void operator=(const OtDaemonServer &) = delete;

    // Dump information for debugging.
    binder_status_t dump(int aFd, const char **aArgs, uint32_t aNumArgs) override;

    static OtDaemonServer *Get(void) { return sOtDaemonServer; }

private:
    using LeaveCallback = std::function<void()>;

    otInstance *GetOtInstance(void);

    // Implements vendor::VendorServer

    void Init(void) override;

    // Implements MainloopProcessor

    void Update(MainloopContext &aMainloop) override;
    void Process(const MainloopContext &aMainloop) override;

    // Creates AndroidThreadHost instance
    std::unique_ptr<AndroidThreadHost> CreateAndroidHost(void);

    // Implements IOtDaemon.aidl

    Status initialize(const bool                                aEnabled,
                      const OtDaemonConfiguration              &aConfiguration,
                      const ScopedFileDescriptor               &aTunFd,
                      const std::shared_ptr<INsdPublisher>     &aINsdPublisher,
                      const MeshcopTxtAttributes               &aMeshcopTxts,
                      const std::string                        &aCountryCode,
                      const bool                                aTrelEnabled,
                      const std::shared_ptr<IOtDaemonCallback> &aCallback) override;
    void   initializeInternal(const bool                                aEnabled,
                              const OtDaemonConfiguration              &aConfiguration,
                              const std::shared_ptr<INsdPublisher>     &aINsdPublisher,
                              const MeshcopTxtAttributes               &aMeshcopTxts,
                              const std::string                        &aCountryCode,
                              const bool                                aTrelEnabled,
                              const std::shared_ptr<IOtDaemonCallback> &aCallback);
    Status terminate(void) override;
    Status setThreadEnabled(const bool aEnabled, const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   setThreadEnabledInternal(const bool aEnabled, const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status registerStateCallback(const std::shared_ptr<IOtDaemonCallback> &aCallback, int64_t aListenerId) override;
    void   registerStateCallbackInternal(const std::shared_ptr<IOtDaemonCallback> &aCallback, int64_t aListenerId);
    bool   isAttached(void);
    Status join(const std::vector<uint8_t>               &aActiveOpDatasetTlvs,
                const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   joinInternal(const std::vector<uint8_t>               &aActiveOpDatasetTlvs,
                        const std::shared_ptr<IOtStatusReceiver> &aReceiver);

    Status leave(bool aEraseDataset, const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   leaveInternal(bool aEraseDataset, const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    void   LeaveGracefully(bool aEraseDataset, const std::string &aCallerTag, const LeaveCallback &aReceiver);
    void   ResetRuntimeStatesAfterLeave();

    Status scheduleMigration(const std::vector<uint8_t>               &aPendingOpDatasetTlvs,
                             const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   scheduleMigrationInternal(const std::vector<uint8_t>               &aPendingOpDatasetTlvs,
                                     const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status setCountryCode(const std::string &aCountryCode, const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    void   setCountryCodeInternal(const std::string &aCountryCode, const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status setChannelMaxPowers(const std::vector<ChannelMaxPower>       &aChannelMaxPowers,
                               const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status setChannelMaxPowersInternal(const std::vector<ChannelMaxPower>       &aChannelMaxPowers,
                                       const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status setConfiguration(const OtDaemonConfiguration              &aConfiguration,
                            const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    Status setInfraLinkInterfaceName(const std::optional<std::string>         &aInterfaceName,
                                     const ScopedFileDescriptor               &aIcmp6Socket,
                                     const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    Status setInfraLinkNat64Prefix(const std::optional<std::string>         &aNat64Prefix,
                                   const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   setInfraLinkNat64PrefixInternal(const std::string                        &aNat64Prefix,
                                           const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status setNat64Cidr(const std::optional<std::string>         &aNat64Cidr,
                        const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   setNat64CidrInternal(const std::optional<std::string>         &aNat64Cidr,
                                const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status setInfraLinkDnsServers(const std::vector<std::string>           &aDnsServers,
                                  const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    void   setInfraLinkDnsServersInternal(const std::vector<std::string>           &aDnsServers,
                                          const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status getChannelMasks(const std::shared_ptr<IChannelMasksReceiver> &aReceiver) override;
    void   getChannelMasksInternal(const std::shared_ptr<IChannelMasksReceiver> &aReceiver);
    Status runOtCtlCommand(const std::string                        &aCommand,
                           const bool                                aIsInteractive,
                           const std::shared_ptr<IOtOutputReceiver> &aReceiver);
    void   runOtCtlCommandInternal(const std::string                        &aCommand,
                                   const bool                                aIsInteractive,
                                   const std::shared_ptr<IOtOutputReceiver> &aReceiver);
    Status activateEphemeralKeyMode(const int64_t                             aLifetimeMillis,
                                    const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   activateEphemeralKeyModeInternal(const int64_t                             aLifetimeMillis,
                                            const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status deactivateEphemeralKeyMode(const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   deactivateEphemeralKeyModeInternal(const std::shared_ptr<IOtStatusReceiver> &aReceiver);

    bool        RefreshOtDaemonState(otChangedFlags aFlags);
    static void DetachGracefullyCallback(void *aBinderServer);
    void        DetachGracefullyCallback(void);
    static void SendMgmtPendingSetCallback(otError aResult, void *aBinderServer);

    static void         BinderDeathCallback(void *aBinderServer);
    void                StateCallback(otChangedFlags aFlags);
    static void         AddressCallback(const otIp6AddressInfo *aAddressInfo, bool aIsAdded, void *aBinderServer);
    static void         ReceiveCallback(otMessage *aMessage, void *aBinderServer);
    void                ReceiveCallback(otMessage *aMessage);
    void                TransmitCallback(void);
    BackboneRouterState GetBackboneRouterState(void);
    static void         HandleBackboneMulticastListenerEvent(void                                  *aBinderServer,
                                                             otBackboneRouterMulticastListenerEvent aEvent,
                                                             const otIp6Address                    *aAddress);
    void                PushTelemetryIfConditionMatch();
    bool                RefreshOnMeshPrefixes();
    Ipv6AddressInfo     ConvertToAddressInfo(const otNetifAddress &aAddress);
    Ipv6AddressInfo     ConvertToAddressInfo(const otNetifMulticastAddress &aAddress);
    void        UpdateThreadEnabledState(const int aEnabled, const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    void        EnableThread(const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    static void HandleEpskcStateChanged(void *aBinderServer);
    void        HandleEpskcStateChanged(void);
    int         GetEphemeralKeyState(void);
    void        NotifyStateChanged(int64_t aListenerId);

    static OtDaemonServer *sOtDaemonServer;

    // Class dependencies
    otbr::Host::RcpHost               &mHost;
    std::unique_ptr<AndroidThreadHost> mAndroidHost;
    MdnsPublisher                     &mMdnsPublisher;
    otbr::BorderAgent                 &mBorderAgent;
    otbr::AdvertisingProxy            &mAdvProxy;
    ResetThreadHandler                 mResetThreadHandler;
    TaskRunner                         mTaskRunner;

    // States initialized in initialize()
    ScopedFileDescriptor               mTunFd;
    std::shared_ptr<INsdPublisher>     mINsdPublisher;
    MeshcopTxtAttributes               mMeshcopTxts;
    std::string                        mCountryCode;
    bool                               mTrelEnabled = false;
    std::shared_ptr<IOtDaemonCallback> mCallback;
    BinderDeathRecipient               mClientDeathRecipient;

    // Runtime states
    std::shared_ptr<IOtStatusReceiver> mJoinReceiver;
    std::shared_ptr<IOtStatusReceiver> mMigrationReceiver;
    std::vector<LeaveCallback>         mLeaveCallbacks;
    OtDaemonState                      mState;
    std::set<OnMeshPrefixConfig>       mOnMeshPrefixes;
    int64_t                            mEphemeralKeyExpiryMillis;

    static constexpr Seconds kTelemetryCheckInterval           = Seconds(600);          // 600 seconds
    static constexpr Seconds kTelemetryUploadIntervalThreshold = Seconds(60 * 60 * 12); // 12 hours
};

} // namespace Android
} // namespace otbr

#endif // OTBR_ANDROID_BINDER_SERVER_HPP_
