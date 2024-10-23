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

#include <aidl/com/android/server/thread/openthread/BnOtDaemon.h>
#include <aidl/com/android/server/thread/openthread/INsdPublisher.h>
#include <aidl/com/android/server/thread/openthread/IOtDaemon.h>
#include <aidl/com/android/server/thread/openthread/InfraLinkState.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>

#include "agent/vendor.hpp"
#include "android/mdns_publisher.hpp"
#include "common/mainloop.hpp"
#include "common/time.hpp"
#include "ncp/rcp_host.hpp"

namespace otbr {
namespace Android {

using BinderDeathRecipient = ::ndk::ScopedAIBinder_DeathRecipient;
using ScopedFileDescriptor = ::ndk::ScopedFileDescriptor;
using Status               = ::ndk::ScopedAStatus;
using aidl::android::net::thread::ChannelMaxPower;
using aidl::com::android::server::thread::openthread::BackboneRouterState;
using aidl::com::android::server::thread::openthread::BnOtDaemon;
using aidl::com::android::server::thread::openthread::IChannelMasksReceiver;
using aidl::com::android::server::thread::openthread::InfraLinkState;
using aidl::com::android::server::thread::openthread::INsdPublisher;
using aidl::com::android::server::thread::openthread::IOtDaemon;
using aidl::com::android::server::thread::openthread::IOtDaemonCallback;
using aidl::com::android::server::thread::openthread::IOtOutputReceiver;
using aidl::com::android::server::thread::openthread::IOtStatusReceiver;
using aidl::com::android::server::thread::openthread::Ipv6AddressInfo;
using aidl::com::android::server::thread::openthread::MeshcopTxtAttributes;
using aidl::com::android::server::thread::openthread::OnMeshPrefixConfig;
using aidl::com::android::server::thread::openthread::OtDaemonConfiguration;
using aidl::com::android::server::thread::openthread::OtDaemonState;

class OtDaemonServer : public BnOtDaemon, public MainloopProcessor, public vendor::VendorServer
{
public:
    OtDaemonServer(otbr::Ncp::RcpHost &rcpHost, otbr::Mdns::Publisher &mdnsPublisher, otbr::BorderAgent &borderAgent);
    virtual ~OtDaemonServer(void) = default;

    // Disallow copy and assign.
    OtDaemonServer(const OtDaemonServer &) = delete;
    void operator=(const OtDaemonServer &) = delete;

    // Dump information for debugging.
    binder_status_t dump(int aFd, const char **aArgs, uint32_t aNumArgs) override;

    static OtDaemonServer *Get(void) { return sOtDaemonServer; }

    void NotifyNat64PrefixDiscoveryDone(void);

private:
    using LeaveCallback = std::function<void()>;

    otInstance *GetOtInstance(void);

    // Implements vendor::VendorServer

    void Init(void) override;

    // Implements MainloopProcessor

    void Update(MainloopContext &aMainloop) override;
    void Process(const MainloopContext &aMainloop) override;

    // Implements IOtDaemon.aidl

    Status initialize(const ScopedFileDescriptor               &aTunFd,
                      const bool                                enabled,
                      const OtDaemonConfiguration              &aConfig,
                      const std::shared_ptr<INsdPublisher>     &aNsdPublisher,
                      const MeshcopTxtAttributes               &aMeshcopTxts,
                      const std::shared_ptr<IOtDaemonCallback> &aCallback,
                      const std::string                        &aCountryCode) override;
    void   initializeInternal(const bool                                enabled,
                              const OtDaemonConfiguration              &aConfig,
                              const std::shared_ptr<INsdPublisher>     &aINsdPublisher,
                              const MeshcopTxtAttributes               &aMeshcopTxts,
                              const std::shared_ptr<IOtDaemonCallback> &aCallback,
                              const std::string                        &aCountryCode);
    Status terminate(void) override;
    Status setThreadEnabled(const bool enabled, const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   setThreadEnabledInternal(const bool enabled, const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status registerStateCallback(const std::shared_ptr<IOtDaemonCallback> &aCallback, int64_t listenerId) override;
    void   registerStateCallbackInternal(const std::shared_ptr<IOtDaemonCallback> &aCallback, int64_t listenerId);
    bool   isAttached(void);
    Status join(const std::vector<uint8_t>               &aActiveOpDatasetTlvs,
                const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   joinInternal(const std::vector<uint8_t>               &aActiveOpDatasetTlvs,
                        const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status leave(const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   leaveInternal(const std::shared_ptr<IOtStatusReceiver> &aReceiver);
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
    void   setConfigurationInternal(const OtDaemonConfiguration              &aConfiguration,
                                    const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status setInfraLinkInterfaceName(const std::optional<std::string>         &aInterfaceName,
                                     const ScopedFileDescriptor               &aIcmp6Socket,
                                     const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   setInfraLinkInterfaceNameInternal(const std::string                        &aInterfaceName,
                                             int                                       aIcmp6SocketFd,
                                             const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status setInfraLinkNat64Prefix(const std::optional<std::string>         &aNat64Prefix,
                                   const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   setInfraLinkNat64PrefixInternal(const std::string                        &aNat64Prefix,
                                           const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status setNat64Cidr(const std::optional<std::string>         &aNat64Cidr,
                        const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   setNat64CidrInternal(const std::optional<std::string>         &aNat64Cidr,
                                const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status getChannelMasks(const std::shared_ptr<IChannelMasksReceiver> &aReceiver) override;
    void   getChannelMasksInternal(const std::shared_ptr<IChannelMasksReceiver> &aReceiver);
    Status runOtCtlCommand(const std::string                        &aCommand,
                           const bool                                aIsInteractive,
                           const std::shared_ptr<IOtOutputReceiver> &aReceiver);
    void   runOtCtlCommandInternal(const std::string                        &aCommand,
                                   const bool                                aIsInteractive,
                                   const std::shared_ptr<IOtOutputReceiver> &aReceiver);
    Status activateEphemeralKeyMode(const int64_t                             lifetimeMillis,
                                    const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   activateEphemeralKeyModeInternal(const int64_t                             lifetimeMillis,
                                            const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    Status deactivateEphemeralKeyMode(const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   deactivateEphemeralKeyModeInternal(const std::shared_ptr<IOtStatusReceiver> &aReceiver);

    bool        RefreshOtDaemonState(otChangedFlags aFlags);
    void        LeaveGracefully(const LeaveCallback &aReceiver);
    void        FinishLeave(const std::shared_ptr<IOtStatusReceiver> &aReceiver);
    static void DetachGracefullyCallback(void *aBinderServer);
    void        DetachGracefullyCallback(void);
    static void SendMgmtPendingSetCallback(otError aResult, void *aBinderServer);

    static void         BinderDeathCallback(void *aBinderServer);
    void                StateCallback(otChangedFlags aFlags);
    static void         AddressCallback(const otIp6AddressInfo *aAddressInfo, bool aIsAdded, void *aBinderServer);
    static void         ReceiveCallback(otMessage *aMessage, void *aBinderServer);
    void                ReceiveCallback(otMessage *aMessage);
    static int          OtCtlCommandCallback(void *aBinderServer, const char *aFormat, va_list aArguments);
    int                 OtCtlCommandCallback(const char *aFormat, va_list aArguments);
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

    otbr::Ncp::RcpHost                &mHost;
    MdnsPublisher                     &mMdnsPublisher;
    otbr::BorderAgent                 &mBorderAgent;
    std::shared_ptr<INsdPublisher>     mINsdPublisher;
    MeshcopTxtAttributes               mMeshcopTxts;
    TaskRunner                         mTaskRunner;
    ScopedFileDescriptor               mTunFd;
    OtDaemonState                      mState;
    std::shared_ptr<IOtDaemonCallback> mCallback;
    BinderDeathRecipient               mClientDeathRecipient;
    std::shared_ptr<IOtStatusReceiver> mJoinReceiver;
    std::shared_ptr<IOtStatusReceiver> mMigrationReceiver;
    std::vector<LeaveCallback>         mLeaveCallbacks;
    bool                               mIsOtCtlInteractiveMode;
    bool                               mIsOtCtlOutputComplete;
    std::shared_ptr<IOtOutputReceiver> mOtCtlOutputReceiver;
    OtDaemonConfiguration              mConfiguration;
    std::set<OnMeshPrefixConfig>       mOnMeshPrefixes;
    InfraLinkState                     mInfraLinkState;
    int                                mInfraIcmp6Socket;
    int64_t                            mEphemeralKeyExpiryMillis;

    static constexpr Seconds kTelemetryCheckInterval           = Seconds(600);          // 600 seconds
    static constexpr Seconds kTelemetryUploadIntervalThreshold = Seconds(60 * 60 * 12); // 12 hours
};

} // namespace Android
} // namespace otbr

#endif // OTBR_ANDROID_BINDER_SERVER_HPP_
