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

#include <linux/in.h>
#define OTBR_LOG_TAG "BINDER"

#include "android/otdaemon_server.hpp"

#include <algorithm>
#include <net/if.h>
#include <random>
#include <string.h>

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <openthread/border_agent.h>
#include <openthread/border_router.h>
#include <openthread/cli.h>
#include <openthread/dnssd_server.h>
#include <openthread/icmp6.h>
#include <openthread/ip6.h>
#include <openthread/link.h>
#include <openthread/nat64.h>
#include <openthread/openthread-system.h>
#include <openthread/srp_server.h>
#include <openthread/platform/infra_if.h>
#include <openthread/platform/radio.h>

#include "agent/vendor.hpp"
#include "android/otdaemon_telemetry.hpp"
#include "common/code_utils.hpp"
#include "ncp/thread_host.hpp"

#define BYTE_ARR_END(arr) ((arr) + sizeof(arr))

namespace otbr {

namespace vendor {

std::shared_ptr<VendorServer> VendorServer::newInstance(Application &aApplication)
{
    return ndk::SharedRefBase::make<Android::OtDaemonServer>(
        static_cast<otbr::Ncp::RcpHost &>(aApplication.GetHost()),
        static_cast<otbr::Android::MdnsPublisher &>(aApplication.GetPublisher()), aApplication.GetBorderAgent());
}

} // namespace vendor

} // namespace otbr

namespace otbr {
namespace Android {

static const char       OTBR_SERVICE_NAME[] = "ot_daemon";
static constexpr size_t kMaxIp6Size         = 1280;

static void PropagateResult(int                                       aError,
                            const std::string                        &aMessage,
                            const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    if (aReceiver != nullptr)
    {
        // If an operation has already been requested or accepted, consider it succeeded
        if (aError == OT_ERROR_NONE || aError == OT_ERROR_ALREADY)
        {
            aReceiver->onSuccess();
        }
        else
        {
            aReceiver->onError(aError, aMessage);
        }
    }
}

static const char *ThreadEnabledStateToString(int enabledState)
{
    switch (enabledState)
    {
    case IOtDaemon::OT_STATE_ENABLED:
        return "ENABLED";
    case IOtDaemon::OT_STATE_DISABLED:
        return "DISABLED";
    case IOtDaemon::OT_STATE_DISABLING:
        return "DISABLING";
    default:
        assert(false);
        return "UNKNOWN";
    }
}

OtDaemonServer *OtDaemonServer::sOtDaemonServer = nullptr;

OtDaemonServer::OtDaemonServer(otbr::Ncp::RcpHost    &rcpHost,
                               otbr::Mdns::Publisher &mdnsPublisher,
                               otbr::BorderAgent     &borderAgent)
    : mHost(rcpHost)
    , mMdnsPublisher(static_cast<MdnsPublisher &>(mdnsPublisher))
    , mBorderAgent(borderAgent)
    , mConfiguration()
{
    mClientDeathRecipient =
        ::ndk::ScopedAIBinder_DeathRecipient(AIBinder_DeathRecipient_new(&OtDaemonServer::BinderDeathCallback));
    mInfraLinkState.interfaceName = "";
    mInfraIcmp6Socket             = -1;
    sOtDaemonServer               = this;
}

void OtDaemonServer::Init(void)
{
    binder_exception_t exp = AServiceManager_registerLazyService(asBinder().get(), OTBR_SERVICE_NAME);
    SuccessOrDie(exp, "Failed to register OT daemon binder service");

    assert(GetOtInstance() != nullptr);

    mHost.AddThreadStateChangedCallback([this](otChangedFlags aFlags) { StateCallback(aFlags); });
    otIp6SetAddressCallback(GetOtInstance(), OtDaemonServer::AddressCallback, this);
    otIp6SetReceiveCallback(GetOtInstance(), OtDaemonServer::ReceiveCallback, this);
    otBackboneRouterSetMulticastListenerCallback(GetOtInstance(), OtDaemonServer::HandleBackboneMulticastListenerEvent,
                                                 this);
    otIcmp6SetEchoMode(GetOtInstance(), OT_ICMP6_ECHO_HANDLER_DISABLED);
    otIp6SetReceiveFilterEnabled(GetOtInstance(), true);
    otNat64SetReceiveIp4Callback(GetOtInstance(), &OtDaemonServer::ReceiveCallback, this);
    mBorderAgent.AddEphemeralKeyChangedCallback([this]() { HandleEpskcStateChanged(); });
    mBorderAgent.SetEphemeralKeyEnabled(true);
    otSysUpstreamDnsServerSetResolvConfEnabled(false);

    mTaskRunner.Post(kTelemetryCheckInterval, [this]() { PushTelemetryIfConditionMatch(); });
}

void OtDaemonServer::BinderDeathCallback(void *aBinderServer)
{
    OtDaemonServer *thisServer = static_cast<OtDaemonServer *>(aBinderServer);

    otbrLogCrit("system_server is dead, removing configs and callbacks...");

    thisServer->mMeshcopTxts   = {};
    thisServer->mINsdPublisher = nullptr;

    // Note that the INsdPublisher reference is held in MdnsPublisher
    thisServer->mMdnsPublisher.SetINsdPublisher(nullptr);

    thisServer->mCallback = nullptr;
    thisServer->mTunFd.set(-1); // the original FD will be closed automatically
}

void OtDaemonServer::StateCallback(otChangedFlags aFlags)
{
    std::vector<OnMeshPrefixConfig> onMeshPrefixes;

    assert(GetOtInstance() != nullptr);

    if (RefreshOtDaemonState(aFlags))
    {
        if (mCallback == nullptr)
        {
            otbrLogWarning("Ignoring OT state changes: callback is not set");
        }
        else
        {
            NotifyStateChanged(/* aListenerId*/ -1);
        }
    }

    if (aFlags & OT_CHANGED_THREAD_BACKBONE_ROUTER_STATE)
    {
        if (mCallback == nullptr)
        {
            otbrLogWarning("Ignoring OT backbone router state changes: callback is not set");
        }
        else
        {
            mCallback->onBackboneRouterStateChanged(GetBackboneRouterState());
        }
    }

    if ((aFlags & OT_CHANGED_THREAD_NETDATA) && RefreshOnMeshPrefixes())
    {
        if (mCallback == nullptr)
        {
            otbrLogWarning("Ignoring OT netdata changes: callback is not set");
        }
        else
        {
            onMeshPrefixes.assign(mOnMeshPrefixes.begin(), mOnMeshPrefixes.end());
            mCallback->onPrefixChanged(onMeshPrefixes);
        }
    }
}

bool OtDaemonServer::RefreshOnMeshPrefixes()
{
    std::set<OnMeshPrefixConfig> onMeshPrefixConfigs;
    otNetworkDataIterator        iterator = OT_NETWORK_DATA_ITERATOR_INIT;
    otBorderRouterConfig         config;
    bool                         rv = false;

    VerifyOrExit(GetOtInstance() != nullptr, otbrLogWarning("Can't get on mesh prefixes: OT is not initialized"));

    while (otNetDataGetNextOnMeshPrefix(GetOtInstance(), &iterator, &config) == OT_ERROR_NONE)
    {
        OnMeshPrefixConfig onMeshPrefixConfig;

        onMeshPrefixConfig.prefix.assign(std::begin(config.mPrefix.mPrefix.mFields.m8),
                                         std::end(config.mPrefix.mPrefix.mFields.m8));
        onMeshPrefixConfig.prefixLength = config.mPrefix.mLength;
        onMeshPrefixConfigs.insert(onMeshPrefixConfig);
    }

    if (mOnMeshPrefixes != onMeshPrefixConfigs)
    {
        mOnMeshPrefixes = std::move(onMeshPrefixConfigs);
        rv              = true;
    }
exit:
    return rv;
}

Ipv6AddressInfo OtDaemonServer::ConvertToAddressInfo(const otNetifAddress &aAddress)
{
    Ipv6AddressInfo addrInfo;
    otIp6Prefix     addressPrefix{aAddress.mAddress, aAddress.mPrefixLength};

    addrInfo.address.assign(std::begin(aAddress.mAddress.mFields.m8), std::end(aAddress.mAddress.mFields.m8));
    addrInfo.prefixLength = aAddress.mPrefixLength;
    addrInfo.isPreferred  = aAddress.mPreferred;
    addrInfo.isMeshLocal  = aAddress.mMeshLocal;
    addrInfo.isActiveOmr  = otNetDataContainsOmrPrefix(GetOtInstance(), &addressPrefix);
    return addrInfo;
}

Ipv6AddressInfo OtDaemonServer::ConvertToAddressInfo(const otNetifMulticastAddress &aAddress)
{
    Ipv6AddressInfo addrInfo;

    addrInfo.address.assign(std::begin(aAddress.mAddress.mFields.m8), std::end(aAddress.mAddress.mFields.m8));
    return addrInfo;
}

void OtDaemonServer::AddressCallback(const otIp6AddressInfo *aAddressInfo, bool aIsAdded, void *aBinderServer)
{
    OT_UNUSED_VARIABLE(aAddressInfo);
    OT_UNUSED_VARIABLE(aIsAdded);
    OtDaemonServer                *thisServer = static_cast<OtDaemonServer *>(aBinderServer);
    std::vector<Ipv6AddressInfo>   addrInfoList;
    const otNetifAddress          *unicastAddrs   = otIp6GetUnicastAddresses(thisServer->GetOtInstance());
    const otNetifMulticastAddress *multicastAddrs = otIp6GetMulticastAddresses(thisServer->GetOtInstance());

    for (const otNetifAddress *addr = unicastAddrs; addr != nullptr; addr = addr->mNext)
    {
        addrInfoList.push_back(thisServer->ConvertToAddressInfo(*addr));
    }
    for (const otNetifMulticastAddress *maddr = multicastAddrs; maddr != nullptr; maddr = maddr->mNext)
    {
        addrInfoList.push_back(thisServer->ConvertToAddressInfo(*maddr));
    }
    if (thisServer->mCallback != nullptr)
    {
        thisServer->mCallback->onAddressChanged(addrInfoList);
    }
    else
    {
        otbrLogWarning("OT daemon callback is not set");
    }
}

void OtDaemonServer::ReceiveCallback(otMessage *aMessage, void *aBinderServer)
{
    static_cast<OtDaemonServer *>(aBinderServer)->ReceiveCallback(aMessage);
}

// TODO: b/291053118 - We should reuse the same code in openthread/src/posix/platform/netif.cpp
void OtDaemonServer::ReceiveCallback(otMessage *aMessage)
{
    char     packet[kMaxIp6Size];
    uint16_t length = otMessageGetLength(aMessage);
    int      fd     = mTunFd.get();

    VerifyOrExit(fd != -1, otbrLogWarning("Ignoring egress packet: invalid tunnel FD"));

    if (otMessageRead(aMessage, 0, packet, sizeof(packet)) != length)
    {
        otbrLogWarning("Failed to read packet from otMessage");
        ExitNow();
    }

    if (write(fd, packet, length) != length)
    {
        otbrLogWarning("Failed to send packet over tunnel interface: %s", strerror(errno));
    }

exit:
    otMessageFree(aMessage);
}

int OtDaemonServer::OtCtlCommandCallback(void *aBinderServer, const char *aFormat, va_list aArguments)
{
    return static_cast<OtDaemonServer *>(aBinderServer)->OtCtlCommandCallback(aFormat, aArguments);
}

int OtDaemonServer::OtCtlCommandCallback(const char *aFormat, va_list aArguments)
{
    static const std::string kPrompt = "> ";
    std::string              output;

    VerifyOrExit(mOtCtlOutputReceiver != nullptr, otSysCliInitUsingDaemon(GetOtInstance()));

    android::base::StringAppendV(&output, aFormat, aArguments);

    // Ignore CLI prompt
    VerifyOrExit(output != kPrompt);

    mOtCtlOutputReceiver->onOutput(output);

    // Check if the command has completed (indicated by "Done" or "Error")
    if (output.starts_with("Done") || output.starts_with("Error"))
    {
        mIsOtCtlOutputComplete = true;
    }

    // The OpenThread CLI consistently outputs "\r\n" as a newline character. Therefore, we use the presence of "\r\n"
    // following "Done" or "Error" to signal the completion of a command's output.
    if (mIsOtCtlOutputComplete && output.ends_with("\r\n"))
    {
        if (!mIsOtCtlInteractiveMode)
        {
            otSysCliInitUsingDaemon(GetOtInstance());
        }
        mIsOtCtlOutputComplete = false;
        mOtCtlOutputReceiver->onComplete();
    }

exit:
    return output.length();
}

static constexpr uint8_t kIpVersion4 = 4;
static constexpr uint8_t kIpVersion6 = 6;

// TODO: b/291053118 - We should reuse the same code in openthread/src/posix/platform/netif.cpp
static uint8_t getIpVersion(const uint8_t *data)
{
    assert(data != nullptr);

    // Mute compiler warnings.
    OT_UNUSED_VARIABLE(kIpVersion4);
    OT_UNUSED_VARIABLE(kIpVersion6);

    return (static_cast<uint8_t>(data[0]) >> 4) & 0x0F;
}

// TODO: b/291053118 - we should use a shared library with ot-posix to handle packet translations
// between the tunnel interface and Thread.
void OtDaemonServer::TransmitCallback(void)
{
    char              packet[kMaxIp6Size];
    ssize_t           length;
    otMessage        *message = nullptr;
    otError           error   = OT_ERROR_NONE;
    otMessageSettings settings;
    int               fd = mTunFd.get();
    bool              isIp4;

    assert(GetOtInstance() != nullptr);

    VerifyOrExit(fd != -1);

    length = read(fd, packet, sizeof(packet));

    if (length == -1)
    {
        otbrLogWarning("Failed to read packet from tunnel interface: %s", strerror(errno));
        ExitNow();
    }
    else if (length == 0)
    {
        otbrLogWarning("Unexpected EOF on the tunnel FD");
        ExitNow();
    }

    VerifyOrExit(GetOtInstance() != nullptr, otbrLogWarning("Ignoring tunnel packet: OT is not initialized"));

    settings.mLinkSecurityEnabled = (otThreadGetDeviceRole(GetOtInstance()) != OT_DEVICE_ROLE_DISABLED);
    settings.mPriority            = OT_MESSAGE_PRIORITY_LOW;

    isIp4   = (getIpVersion(reinterpret_cast<uint8_t *>(packet)) == kIpVersion4);
    message = isIp4 ? otIp4NewMessage(GetOtInstance(), &settings) : otIp6NewMessage(GetOtInstance(), &settings);
    VerifyOrExit(message != nullptr, error = OT_ERROR_NO_BUFS);
    otMessageSetOrigin(message, OT_MESSAGE_ORIGIN_HOST_UNTRUSTED);

    SuccessOrExit(error = otMessageAppend(message, packet, static_cast<uint16_t>(length)));

    error   = isIp4 ? otNat64Send(GetOtInstance(), message) : otIp6Send(GetOtInstance(), message);
    message = nullptr;

exit:
    if (message != nullptr)
    {
        otMessageFree(message);
    }

    if (error != OT_ERROR_NONE)
    {
        if (error == OT_ERROR_DROP)
        {
            otbrLogInfo("Dropped tunnel packet (length=%d)", length);
        }
        else
        {
            otbrLogWarning("Failed to transmit tunnel packet: %s", otThreadErrorToString(error));
        }
    }
}

void OtDaemonServer::HandleEpskcStateChanged(void *aBinderServer)
{
    static_cast<OtDaemonServer *>(aBinderServer)->HandleEpskcStateChanged();
}

void OtDaemonServer::HandleEpskcStateChanged(void)
{
    mState.ephemeralKeyState = GetEphemeralKeyState();

    NotifyStateChanged(/* aListenerId*/ -1);
}

void OtDaemonServer::NotifyStateChanged(int64_t aListenerId)
{
    if (mState.ephemeralKeyState == OT_EPHEMERAL_KEY_DISABLED)
    {
        mState.ephemeralKeyLifetimeMillis = 0;
    }
    else
    {
        mState.ephemeralKeyLifetimeMillis =
            mEphemeralKeyExpiryMillis -
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
    }
    if (mCallback != nullptr)
    {
        mCallback->onStateChanged(mState, aListenerId);
    }
}

int OtDaemonServer::GetEphemeralKeyState(void)
{
    int ephemeralKeyState;

    if (otBorderAgentIsEphemeralKeyActive(GetOtInstance()))
    {
        if (otBorderAgentGetState(GetOtInstance()) == OT_BORDER_AGENT_STATE_ACTIVE)
        {
            ephemeralKeyState = OT_EPHEMERAL_KEY_IN_USE;
        }
        else
        {
            ephemeralKeyState = OT_EPHEMERAL_KEY_ENABLED;
        }
    }
    else
    {
        ephemeralKeyState = OT_EPHEMERAL_KEY_DISABLED;
    }

    return ephemeralKeyState;
}

BackboneRouterState OtDaemonServer::GetBackboneRouterState()
{
    BackboneRouterState                       state;
    otBackboneRouterState                     bbrState;
    otBackboneRouterMulticastListenerInfo     info;
    otBackboneRouterMulticastListenerIterator iter = OT_BACKBONE_ROUTER_MULTICAST_LISTENER_ITERATOR_INIT;
    state.listeningAddresses                       = std::vector<std::string>();

    VerifyOrExit(GetOtInstance() != nullptr, otbrLogWarning("Can't get bbr state: OT is not initialized"));

    bbrState = otBackboneRouterGetState(GetOtInstance());
    switch (bbrState)
    {
    case OT_BACKBONE_ROUTER_STATE_DISABLED:
    case OT_BACKBONE_ROUTER_STATE_SECONDARY:
        state.multicastForwardingEnabled = false;
        break;
    case OT_BACKBONE_ROUTER_STATE_PRIMARY:
        state.multicastForwardingEnabled = true;
        break;
    }

    while (otBackboneRouterMulticastListenerGetNext(GetOtInstance(), &iter, &info) == OT_ERROR_NONE)
    {
        char string[OT_IP6_ADDRESS_STRING_SIZE];

        otIp6AddressToString(&info.mAddress, string, sizeof(string));
        state.listeningAddresses.push_back(string);
    }

exit:
    return state;
}

void OtDaemonServer::HandleBackboneMulticastListenerEvent(void                                  *aBinderServer,
                                                          otBackboneRouterMulticastListenerEvent aEvent,
                                                          const otIp6Address                    *aAddress)
{
    OtDaemonServer *thisServer = static_cast<OtDaemonServer *>(aBinderServer);
    char            addressString[OT_IP6_ADDRESS_STRING_SIZE];

    otIp6AddressToString(aAddress, addressString, sizeof(addressString));

    otbrLogInfo("Multicast forwarding address changed, %s is %s", addressString,
                (aEvent == OT_BACKBONE_ROUTER_MULTICAST_LISTENER_ADDED) ? "added" : "removed");

    if (thisServer->mCallback == nullptr)
    {
        otbrLogWarning("Ignoring OT multicast listener event: callback is not set");
        ExitNow();
    }
    thisServer->mCallback->onBackboneRouterStateChanged(thisServer->GetBackboneRouterState());

exit:
    return;
}

otInstance *OtDaemonServer::GetOtInstance()
{
    return mHost.GetInstance();
}

void OtDaemonServer::Update(MainloopContext &aMainloop)
{
    int fd = mTunFd.get();

    if (fd != -1)
    {
        FD_SET(fd, &aMainloop.mReadFdSet);
        aMainloop.mMaxFd = std::max(aMainloop.mMaxFd, fd);
    }
}

void OtDaemonServer::Process(const MainloopContext &aMainloop)
{
    int fd = mTunFd.get();

    if (fd != -1 && FD_ISSET(fd, &aMainloop.mReadFdSet))
    {
        TransmitCallback();
    }
}

Status OtDaemonServer::initialize(const ScopedFileDescriptor               &aTunFd,
                                  const bool                                aEnabled,
                                  const OtDaemonConfiguration              &aConfiguration,
                                  const std::shared_ptr<INsdPublisher>     &aINsdPublisher,
                                  const MeshcopTxtAttributes               &aMeshcopTxts,
                                  const std::shared_ptr<IOtDaemonCallback> &aCallback,
                                  const std::string                        &aCountryCode)
{
    otbrLogInfo("OT daemon is initialized by system server (enabled=%s, tunFd=%d)", (aEnabled ? "true" : "false"),
                aTunFd.get());

    // The copy constructor of `ScopedFileDescriptor` is deleted. It is unable to pass the `aTunFd`
    // to the lambda function. The processing method of `aTunFd` doesn't call OpenThread functions,
    // we can process `aTunFd` directly in front of the task.
    mTunFd = aTunFd.dup();

    mINsdPublisher = aINsdPublisher;
    mMeshcopTxts   = aMeshcopTxts;

    mTaskRunner.Post([aEnabled, aConfiguration, aINsdPublisher, aMeshcopTxts, aCallback, aCountryCode, this]() {
        initializeInternal(aEnabled, aConfiguration, mINsdPublisher, mMeshcopTxts, aCallback, aCountryCode);
    });

    return Status::ok();
}

void OtDaemonServer::initializeInternal(const bool                                aEnabled,
                                        const OtDaemonConfiguration              &aConfiguration,
                                        const std::shared_ptr<INsdPublisher>     &aINsdPublisher,
                                        const MeshcopTxtAttributes               &aMeshcopTxts,
                                        const std::shared_ptr<IOtDaemonCallback> &aCallback,
                                        const std::string                        &aCountryCode)
{
    std::string              instanceName = aMeshcopTxts.vendorName + " " + aMeshcopTxts.modelName;
    Mdns::Publisher::TxtList nonStandardTxts;
    otbrError                error;

    setConfigurationInternal(aConfiguration, nullptr /* aReceiver */);
    setCountryCodeInternal(aCountryCode, nullptr /* aReceiver */);
    registerStateCallbackInternal(aCallback, -1 /* listenerId */);

    mMdnsPublisher.SetINsdPublisher(aINsdPublisher);

    for (const auto &txtAttr : aMeshcopTxts.nonStandardTxtEntries)
    {
        nonStandardTxts.emplace_back(txtAttr.name.c_str(), txtAttr.value.data(), txtAttr.value.size());
    }
    error = mBorderAgent.SetMeshCopServiceValues(instanceName, aMeshcopTxts.modelName, aMeshcopTxts.vendorName,
                                                 aMeshcopTxts.vendorOui, nonStandardTxts);
    if (error != OTBR_ERROR_NONE)
    {
        otbrLogCrit("Failed to set MeshCoP values: %d", static_cast<int>(error));
    }

    mBorderAgent.SetEnabled(aEnabled && aConfiguration.borderRouterEnabled);

    if (aEnabled)
    {
        EnableThread(nullptr /* aReceiver */);
    }
    else
    {
        UpdateThreadEnabledState(OT_STATE_DISABLED, nullptr /* aReceiver */);
    }
}

Status OtDaemonServer::terminate(void)
{
    mTaskRunner.Post([]() {
        otbrLogWarning("Terminating ot-daemon process...");
        exit(0);
    });
    return Status::ok();
}

void OtDaemonServer::UpdateThreadEnabledState(const int enabled, const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    VerifyOrExit(enabled != mState.threadEnabled);

    otbrLogInfo("Thread enabled state changed: %s -> %s", ThreadEnabledStateToString(mState.threadEnabled),
                ThreadEnabledStateToString(enabled));
    mState.threadEnabled = enabled;

    if (aReceiver != nullptr)
    {
        aReceiver->onSuccess();
    }

    // Enables the BorderAgent module only when Thread is enabled and configured a Border Router,
    // so that it won't publish the MeshCoP mDNS service when unnecessary
    // TODO: b/376217403 - enables / disables OT Border Agent at runtime
    mBorderAgent.SetEnabled(enabled == OT_STATE_ENABLED && mConfiguration.borderRouterEnabled);

    NotifyStateChanged(/* aListenerId*/ -1);

exit:
    return;
}

void OtDaemonServer::EnableThread(const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    otOperationalDatasetTlvs datasetTlvs;

    if (otDatasetGetActiveTlvs(GetOtInstance(), &datasetTlvs) != OT_ERROR_NOT_FOUND && datasetTlvs.mLength > 0 &&
        !isAttached())
    {
        (void)otIp6SetEnabled(GetOtInstance(), true);
        (void)otThreadSetEnabled(GetOtInstance(), true);
    }
    UpdateThreadEnabledState(OT_STATE_ENABLED, aReceiver);
}

Status OtDaemonServer::setThreadEnabled(const bool enabled, const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post([enabled, aReceiver, this]() { setThreadEnabledInternal(enabled, aReceiver); });

    return Status::ok();
}

void OtDaemonServer::setThreadEnabledInternal(const bool enabled, const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    int         error = OT_ERROR_NONE;
    std::string message;

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    VerifyOrExit(mState.threadEnabled != OT_STATE_DISABLING, error = OT_ERROR_BUSY, message = "Thread is disabling");

    if ((mState.threadEnabled == OT_STATE_ENABLED) == enabled)
    {
        aReceiver->onSuccess();
        ExitNow();
    }

    if (enabled)
    {
        EnableThread(aReceiver);
    }
    else
    {
        // `aReceiver` should not be set here because the operation isn't finished yet
        UpdateThreadEnabledState(OT_STATE_DISABLING, nullptr /* aReceiver */);

        LeaveGracefully([aReceiver, this]() {
            // Ignore errors as those operations should always succeed
            (void)otThreadSetEnabled(GetOtInstance(), false);
            (void)otIp6SetEnabled(GetOtInstance(), false);
            UpdateThreadEnabledState(OT_STATE_DISABLED, aReceiver);
        });
    }

exit:
    if (error != OT_ERROR_NONE)
    {
        PropagateResult(error, message, aReceiver);
    }
}

Status OtDaemonServer::activateEphemeralKeyMode(const int64_t                             lifetimeMillis,
                                                const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post(
        [lifetimeMillis, aReceiver, this]() { activateEphemeralKeyModeInternal(lifetimeMillis, aReceiver); });

    return Status::ok();
}

void OtDaemonServer::activateEphemeralKeyModeInternal(const int64_t                             lifetimeMillis,
                                                      const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    int         error = OT_ERROR_NONE;
    std::string message;
    std::string passcode;

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");
    VerifyOrExit(isAttached(), error = static_cast<int>(IOtDaemon::ErrorCode::OT_ERROR_FAILED_PRECONDITION),
                 message = "Cannot activate ephemeral key mode when this device is not attached to Thread network");
    VerifyOrExit(!otBorderAgentIsEphemeralKeyActive(GetOtInstance()), error = OT_ERROR_BUSY,
                 message = "ephemeral key mode is already activated");

    otbrLogInfo("Activating ephemeral key mode with %lldms lifetime.", lifetimeMillis);

    SuccessOrExit(error = mBorderAgent.CreateEphemeralKey(passcode), message = "Failed to create ephemeral key");
    SuccessOrExit(error   = otBorderAgentSetEphemeralKey(GetOtInstance(), passcode.c_str(),
                                                         static_cast<uint32_t>(lifetimeMillis), 0 /* aUdpPort */),
                  message = "Failed to set ephemeral key");

exit:
    if (aReceiver != nullptr)
    {
        if (error == OT_ERROR_NONE)
        {
            mState.ephemeralKeyPasscode = passcode;
            mEphemeralKeyExpiryMillis   = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count() +
                                        lifetimeMillis;
            aReceiver->onSuccess();
        }
        else
        {
            aReceiver->onError(error, message);
        }
    }
}

Status OtDaemonServer::deactivateEphemeralKeyMode(const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post([aReceiver, this]() { deactivateEphemeralKeyModeInternal(aReceiver); });

    return Status::ok();
}

void OtDaemonServer::deactivateEphemeralKeyModeInternal(const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    int         error = OT_ERROR_NONE;
    std::string message;

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");
    otbrLogInfo("Deactivating ephemeral key mode.");

    VerifyOrExit(otBorderAgentIsEphemeralKeyActive(GetOtInstance()), error = OT_ERROR_NONE);

    otBorderAgentDisconnect(GetOtInstance());
    otBorderAgentClearEphemeralKey(GetOtInstance());

exit:
    PropagateResult(error, message, aReceiver);
}

Status OtDaemonServer::registerStateCallback(const std::shared_ptr<IOtDaemonCallback> &aCallback, int64_t listenerId)
{
    mTaskRunner.Post([aCallback, listenerId, this]() { registerStateCallbackInternal(aCallback, listenerId); });

    return Status::ok();
}

void OtDaemonServer::registerStateCallbackInternal(const std::shared_ptr<IOtDaemonCallback> &aCallback,
                                                   int64_t                                   listenerId)
{
    VerifyOrExit(GetOtInstance() != nullptr, otbrLogWarning("OT is not initialized"));

    mCallback = aCallback;
    if (mCallback != nullptr)
    {
        AIBinder_linkToDeath(mCallback->asBinder().get(), mClientDeathRecipient.get(), this);
    }

    // To ensure that a client app can get the latest correct state immediately when registering a
    // state callback, here needs to invoke the callback
    RefreshOtDaemonState(/* aFlags */ 0xffffffff);
    NotifyStateChanged(listenerId);
    mCallback->onBackboneRouterStateChanged(GetBackboneRouterState());

exit:
    return;
}

bool OtDaemonServer::RefreshOtDaemonState(otChangedFlags aFlags)
{
    bool haveUpdates = false;

    if (aFlags & OT_CHANGED_THREAD_NETIF_STATE)
    {
        mState.isInterfaceUp = otIp6IsEnabled(GetOtInstance());
        haveUpdates          = true;
    }

    if (aFlags & OT_CHANGED_THREAD_ROLE)
    {
        mState.deviceRole = otThreadGetDeviceRole(GetOtInstance());
        haveUpdates       = true;
    }

    if (aFlags & OT_CHANGED_THREAD_PARTITION_ID)
    {
        mState.partitionId = otThreadGetPartitionId(GetOtInstance());
        haveUpdates        = true;
    }

    if (aFlags & OT_CHANGED_ACTIVE_DATASET)
    {
        otOperationalDatasetTlvs datasetTlvs;
        if (otDatasetGetActiveTlvs(GetOtInstance(), &datasetTlvs) == OT_ERROR_NONE)
        {
            mState.activeDatasetTlvs.assign(datasetTlvs.mTlvs, datasetTlvs.mTlvs + datasetTlvs.mLength);
        }
        else
        {
            mState.activeDatasetTlvs.clear();
        }
        haveUpdates = true;
    }

    if (aFlags & OT_CHANGED_PENDING_DATASET)
    {
        otOperationalDatasetTlvs datasetTlvs;
        if (otDatasetGetPendingTlvs(GetOtInstance(), &datasetTlvs) == OT_ERROR_NONE)
        {
            mState.pendingDatasetTlvs.assign(datasetTlvs.mTlvs, datasetTlvs.mTlvs + datasetTlvs.mLength);
        }
        else
        {
            mState.pendingDatasetTlvs.clear();
        }
        haveUpdates = true;
    }

    if (isAttached() && !mState.activeDatasetTlvs.empty() && mJoinReceiver != nullptr)
    {
        otbrLogInfo("Join succeeded");
        mJoinReceiver->onSuccess();
        mJoinReceiver = nullptr;
    }

    return haveUpdates;
}

/**
 * Returns `true` if the two TLV lists are representing the same Operational Dataset.
 *
 * Note this method works even if TLVs in `aLhs` and `aRhs` are not ordered.
 */
static bool areDatasetsEqual(const otOperationalDatasetTlvs &aLhs, const otOperationalDatasetTlvs &aRhs)
{
    bool result = false;

    otOperationalDataset     lhsDataset;
    otOperationalDataset     rhsDataset;
    otOperationalDatasetTlvs lhsNormalizedTlvs;
    otOperationalDatasetTlvs rhsNormalizedTlvs;

    // Sort the TLVs in the TLV byte arrays by leveraging the deterministic nature of the two OT APIs
    SuccessOrExit(otDatasetParseTlvs(&aLhs, &lhsDataset));
    SuccessOrExit(otDatasetParseTlvs(&aRhs, &rhsDataset));
    otDatasetConvertToTlvs(&lhsDataset, &lhsNormalizedTlvs);
    otDatasetConvertToTlvs(&rhsDataset, &rhsNormalizedTlvs);

    result = (lhsNormalizedTlvs.mLength == rhsNormalizedTlvs.mLength) &&
             (memcmp(lhsNormalizedTlvs.mTlvs, rhsNormalizedTlvs.mTlvs, lhsNormalizedTlvs.mLength) == 0);

exit:
    return result;
}

Status OtDaemonServer::join(const std::vector<uint8_t>               &aActiveOpDatasetTlvs,
                            const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post([aActiveOpDatasetTlvs, aReceiver, this]() { joinInternal(aActiveOpDatasetTlvs, aReceiver); });

    return Status::ok();
}

void OtDaemonServer::joinInternal(const std::vector<uint8_t>               &aActiveOpDatasetTlvs,
                                  const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    int                      error = OT_ERROR_NONE;
    std::string              message;
    otOperationalDatasetTlvs newDatasetTlvs;
    otOperationalDatasetTlvs curDatasetTlvs;

    VerifyOrExit(mState.threadEnabled != OT_STATE_DISABLING, error = OT_ERROR_BUSY, message = "Thread is disabling");

    VerifyOrExit(mState.threadEnabled == OT_STATE_ENABLED,
                 error   = static_cast<int>(IOtDaemon::ErrorCode::OT_ERROR_THREAD_DISABLED),
                 message = "Thread is disabled");

    otbrLogInfo("Start joining...");

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    std::copy(aActiveOpDatasetTlvs.begin(), aActiveOpDatasetTlvs.end(), newDatasetTlvs.mTlvs);
    newDatasetTlvs.mLength = static_cast<uint8_t>(aActiveOpDatasetTlvs.size());

    error = otDatasetGetActiveTlvs(GetOtInstance(), &curDatasetTlvs);
    if (error == OT_ERROR_NONE && areDatasetsEqual(newDatasetTlvs, curDatasetTlvs) && isAttached())
    {
        // Do not leave and re-join if this device has already joined the same network. This can help elimilate
        // unnecessary connectivity and topology disruption and save the time for re-joining. It's more useful for use
        // cases where Thread networks are dynamically brought up and torn down (e.g. Thread on mobile phones).
        aReceiver->onSuccess();
        ExitNow();
    }

    if (otThreadGetDeviceRole(GetOtInstance()) != OT_DEVICE_ROLE_DISABLED)
    {
        LeaveGracefully([aActiveOpDatasetTlvs, aReceiver, this]() {
            FinishLeave(true /* aEraseDataset */, nullptr);
            join(aActiveOpDatasetTlvs, aReceiver);
        });
        ExitNow();
    }

    SuccessOrExit(error   = otDatasetSetActiveTlvs(GetOtInstance(), &newDatasetTlvs),
                  message = "Failed to set Active Operational Dataset");

    // TODO(b/273160198): check how we can implement join as a child

    // Shouldn't we have an equivalent `otThreadAttach` method vs `otThreadDetachGracefully`?
    SuccessOrExit(error = otIp6SetEnabled(GetOtInstance(), true), message = "Failed to bring up Thread interface");
    SuccessOrExit(error = otThreadSetEnabled(GetOtInstance(), true), message = "Failed to bring up Thread stack");

    // Abort an ongoing join()
    if (mJoinReceiver != nullptr)
    {
        mJoinReceiver->onError(OT_ERROR_ABORT, "Join() is aborted");
    }
    mJoinReceiver = aReceiver;

exit:
    if (error != OT_ERROR_NONE)
    {
        PropagateResult(error, message, aReceiver);
    }
}

Status OtDaemonServer::leave(bool aEraseDataset, const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post([aEraseDataset, aReceiver, this]() { leaveInternal(aEraseDataset, aReceiver); });

    return Status::ok();
}

void OtDaemonServer::leaveInternal(bool aEraseDataset, const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    std::string message;
    int         error = OT_ERROR_NONE;

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    VerifyOrExit(mState.threadEnabled != OT_STATE_DISABLING, error = OT_ERROR_BUSY, message = "Thread is disabling");

    if (mState.threadEnabled == OT_STATE_DISABLED)
    {
        FinishLeave(aEraseDataset, aReceiver);
        ExitNow();
    }

    LeaveGracefully([aEraseDataset, aReceiver, this]() { FinishLeave(aEraseDataset, aReceiver); });

exit:
    if (error != OT_ERROR_NONE)
    {
        PropagateResult(error, message, aReceiver);
    }
}

void OtDaemonServer::FinishLeave(bool aEraseDataset, const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    if (aEraseDataset)
    {
        (void)otInstanceErasePersistentInfo(GetOtInstance());
    }

    // TODO: b/323301831 - Re-init the Application class.
    if (aReceiver != nullptr)
    {
        aReceiver->onSuccess();
    }
}

void OtDaemonServer::LeaveGracefully(const LeaveCallback &aReceiver)
{
    mLeaveCallbacks.push_back(aReceiver);

    // Ignores the OT_ERROR_BUSY error if a detach has already been requested
    (void)otThreadDetachGracefully(GetOtInstance(), DetachGracefullyCallback, this);
}

void OtDaemonServer::DetachGracefullyCallback(void *aBinderServer)
{
    OtDaemonServer *thisServer = static_cast<OtDaemonServer *>(aBinderServer);
    thisServer->DetachGracefullyCallback();
}

void OtDaemonServer::DetachGracefullyCallback(void)
{
    otbrLogInfo("detach success...");

    if (mJoinReceiver != nullptr)
    {
        mJoinReceiver->onError(OT_ERROR_ABORT, "Aborted by leave/disable operation");
        mJoinReceiver = nullptr;
    }

    if (mMigrationReceiver != nullptr)
    {
        mMigrationReceiver->onError(OT_ERROR_ABORT, "Aborted by leave/disable operation");
        mMigrationReceiver = nullptr;
    }

    for (auto &callback : mLeaveCallbacks)
    {
        callback();
    }
    mLeaveCallbacks.clear();
}

bool OtDaemonServer::isAttached()
{
    otDeviceRole role = otThreadGetDeviceRole(GetOtInstance());

    return role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
}

Status OtDaemonServer::scheduleMigration(const std::vector<uint8_t>               &aPendingOpDatasetTlvs,
                                         const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post(
        [aPendingOpDatasetTlvs, aReceiver, this]() { scheduleMigrationInternal(aPendingOpDatasetTlvs, aReceiver); });

    return Status::ok();
}

void OtDaemonServer::scheduleMigrationInternal(const std::vector<uint8_t>               &aPendingOpDatasetTlvs,
                                               const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    int                  error = OT_ERROR_NONE;
    std::string          message;
    otOperationalDataset emptyDataset;

    VerifyOrExit(mState.threadEnabled != OT_STATE_DISABLING, error = OT_ERROR_BUSY, message = "Thread is disabling");

    VerifyOrExit(mState.threadEnabled == OT_STATE_ENABLED,
                 error   = static_cast<int>(IOtDaemon::ErrorCode::OT_ERROR_THREAD_DISABLED),
                 message = "Thread is disabled");

    if (GetOtInstance() == nullptr)
    {
        message = "OT is not initialized";
        ExitNow(error = OT_ERROR_INVALID_STATE);
    }
    if (!isAttached())
    {
        message = "Cannot schedule migration when this device is detached";
        ExitNow(error = static_cast<int>(IOtDaemon::ErrorCode::OT_ERROR_FAILED_PRECONDITION));
    }

    // TODO: check supported channel mask

    error = otDatasetSendMgmtPendingSet(GetOtInstance(), &emptyDataset, aPendingOpDatasetTlvs.data(),
                                        static_cast<uint8_t>(aPendingOpDatasetTlvs.size()), SendMgmtPendingSetCallback,
                                        /* aBinderServer= */ this);
    if (error != OT_ERROR_NONE)
    {
        message = "Failed to send MGMT_PENDING_SET.req";
    }

exit:
    if (error != OT_ERROR_NONE)
    {
        PropagateResult(error, message, aReceiver);
    }
    else
    {
        // otDatasetSendMgmtPendingSet() returns OT_ERROR_BUSY if it has already been called before but the
        // callback hasn't been invoked. So we can guarantee that mMigrationReceiver is always nullptr here
        assert(mMigrationReceiver == nullptr);
        mMigrationReceiver = aReceiver;
    }
}

void OtDaemonServer::SendMgmtPendingSetCallback(otError aResult, void *aBinderServer)
{
    OtDaemonServer *thisServer = static_cast<OtDaemonServer *>(aBinderServer);

    if (thisServer->mMigrationReceiver != nullptr)
    {
        PropagateResult(aResult, "Failed to register Pending Dataset to leader", thisServer->mMigrationReceiver);
        thisServer->mMigrationReceiver = nullptr;
    }
}

Status OtDaemonServer::setCountryCode(const std::string                        &aCountryCode,
                                      const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post([aCountryCode, aReceiver, this]() { setCountryCodeInternal(aCountryCode, aReceiver); });

    return Status::ok();
}

void OtDaemonServer::setCountryCodeInternal(const std::string                        &aCountryCode,
                                            const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mHost.SetCountryCode(aCountryCode, [aReceiver](otError aError, const std::string &aMessage) {
        PropagateResult(aError, aMessage, aReceiver);
    });
}

Status OtDaemonServer::getChannelMasks(const std::shared_ptr<IChannelMasksReceiver> &aReceiver)
{
    mTaskRunner.Post([aReceiver, this]() { getChannelMasksInternal(aReceiver); });

    return Status::ok();
}

void OtDaemonServer::getChannelMasksInternal(const std::shared_ptr<IChannelMasksReceiver> &aReceiver)
{
    auto channelMasksReceiver = [aReceiver](uint32_t aSupportedChannelMask, uint32_t aPreferredChannelMask) {
        aReceiver->onSuccess(aSupportedChannelMask, aPreferredChannelMask);
    };
    auto errorReceiver = [aReceiver](otError aError, const std::string &aMessage) {
        aReceiver->onError(aError, aMessage);
    };
    mHost.GetChannelMasks(channelMasksReceiver, errorReceiver);
}

Status OtDaemonServer::setChannelMaxPowers(const std::vector<ChannelMaxPower>       &aChannelMaxPowers,
                                           const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post(
        [aChannelMaxPowers, aReceiver, this]() { setChannelMaxPowersInternal(aChannelMaxPowers, aReceiver); });

    return Status::ok();
}

Status OtDaemonServer::setChannelMaxPowersInternal(const std::vector<ChannelMaxPower>       &aChannelMaxPowers,
                                                   const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    // Transform aidl ChannelMaxPower to ThreadHost::ChannelMaxPower
    std::vector<Ncp::ThreadHost::ChannelMaxPower> channelMaxPowers(aChannelMaxPowers.size());
    std::transform(aChannelMaxPowers.begin(), aChannelMaxPowers.end(), channelMaxPowers.begin(),
                   [](const ChannelMaxPower &aChannelMaxPower) {
                       // INT_MIN indicates that the corresponding channel is disabled in Thread Android API
                       // `setChannelMaxPowers()` INT16_MAX indicates that the corresponding channel is disabled in
                       // OpenThread API `otPlatRadioSetChannelTargetPower()`.
                       return Ncp::ThreadHost::ChannelMaxPower(
                           aChannelMaxPower.channel,
                           aChannelMaxPower.maxPower == INT_MIN
                               ? INT16_MAX
                               : std::clamp(aChannelMaxPower.maxPower, INT16_MIN, INT16_MAX - 1));
                   });

    mHost.SetChannelMaxPowers(channelMaxPowers, [aReceiver](otError aError, const std::string &aMessage) {
        PropagateResult(aError, aMessage, aReceiver);
    });

    return Status::ok();
}

Status OtDaemonServer::setConfiguration(const OtDaemonConfiguration              &aConfiguration,
                                        const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post([aConfiguration, aReceiver, this]() { setConfigurationInternal(aConfiguration, aReceiver); });

    return Status::ok();
}

void OtDaemonServer::setConfigurationInternal(const OtDaemonConfiguration              &aConfiguration,
                                              const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    otError          error = OT_ERROR_NONE;
    std::string      message;
    otLinkModeConfig linkModeConfig;

    otbrLogInfo("Set configuration: %s", aConfiguration.toString().c_str());

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");
    VerifyOrExit(aConfiguration != mConfiguration);

    // TODO: b/343814054 - Support enabling/disabling DHCPv6-PD.
    VerifyOrExit(!aConfiguration.dhcpv6PdEnabled, error = OT_ERROR_NOT_IMPLEMENTED,
                 message = "DHCPv6-PD is not supported");
    otNat64SetEnabled(GetOtInstance(), aConfiguration.nat64Enabled);
    // DNS upstream query is enabled if and only if NAT64 is enabled.
    otDnssdUpstreamQuerySetEnabled(GetOtInstance(), aConfiguration.nat64Enabled);

    linkModeConfig = GetLinkModeConfig(aConfiguration.borderRouterEnabled);
    SuccessOrExit(error = otThreadSetLinkMode(GetOtInstance(), linkModeConfig), message = "Failed to set link mode");
    if (aConfiguration.borderRouterEnabled)
    {
        otSrpServerSetAutoEnableMode(GetOtInstance(), true);
    }
    else
    {
        // This automatically disables the auto-enable mode which is designed for border router
        otSrpServerSetEnabled(GetOtInstance(), true);
    }

    mConfiguration = aConfiguration;

exit:
    PropagateResult(error, message, aReceiver);
}

Status OtDaemonServer::setInfraLinkInterfaceName(const std::optional<std::string>         &aInterfaceName,
                                                 const ScopedFileDescriptor               &aIcmp6Socket,
                                                 const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    int icmp6Socket = aIcmp6Socket.dup().release();

    mTaskRunner.Post([interfaceName = aInterfaceName.value_or(""), icmp6Socket, aReceiver, this]() {
        setInfraLinkInterfaceNameInternal(interfaceName, icmp6Socket, aReceiver);
    });

    return Status::ok();
}

void OtDaemonServer::setInfraLinkInterfaceNameInternal(const std::string                        &aInterfaceName,
                                                       int                                       aIcmp6Socket,
                                                       const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    otError           error = OT_ERROR_NONE;
    std::string       message;
    const std::string infraIfName  = aInterfaceName;
    unsigned int      infraIfIndex = if_nametoindex(infraIfName.c_str());

    otbrLogInfo("Setting infra link state: %s", aInterfaceName.c_str());

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");
    VerifyOrExit(mConfiguration.borderRouterEnabled, error = OT_ERROR_INVALID_STATE,
                 message = "Set infra link state when border router is disabled");
    VerifyOrExit(mInfraLinkState.interfaceName != aInterfaceName || aIcmp6Socket != mInfraIcmp6Socket);

    if (infraIfIndex != 0 && aIcmp6Socket > 0)
    {
        SuccessOrExit(error   = otBorderRoutingSetEnabled(GetOtInstance(), false /* aEnabled */),
                      message = "failed to disable border routing");
        otSysSetInfraNetif(infraIfName.c_str(), aIcmp6Socket);
        aIcmp6Socket = -1;
        SuccessOrExit(error   = otBorderRoutingInit(GetOtInstance(), infraIfIndex, otSysInfraIfIsRunning()),
                      message = "failed to initialize border routing");
        SuccessOrExit(error   = otBorderRoutingSetEnabled(GetOtInstance(), true /* aEnabled */),
                      message = "failed to enable border routing");
        // TODO: b/320836258 - Make BBR independently configurable
        otBackboneRouterSetEnabled(GetOtInstance(), true /* aEnabled */);
    }
    else
    {
        SuccessOrExit(error   = otBorderRoutingSetEnabled(GetOtInstance(), false /* aEnabled */),
                      message = "failed to disable border routing");
        otBackboneRouterSetEnabled(GetOtInstance(), false /* aEnabled */);
    }

    mInfraLinkState.interfaceName = aInterfaceName;
    mInfraIcmp6Socket             = aIcmp6Socket;

exit:
    if (error != OT_ERROR_NONE)
    {
        close(aIcmp6Socket);
    }
    PropagateResult(error, message, aReceiver);
}

Status OtDaemonServer::runOtCtlCommand(const std::string                        &aCommand,
                                       const bool                                aIsInteractive,
                                       const std::shared_ptr<IOtOutputReceiver> &aReceiver)
{
    mTaskRunner.Post([aCommand, aIsInteractive, aReceiver, this]() {
        runOtCtlCommandInternal(aCommand, aIsInteractive, aReceiver);
    });

    return Status::ok();
}

Status OtDaemonServer::setInfraLinkNat64Prefix(const std::optional<std::string>         &aNat64Prefix,
                                               const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post([nat64Prefix = aNat64Prefix.value_or(""), aReceiver, this]() {
        setInfraLinkNat64PrefixInternal(nat64Prefix, aReceiver);
    });

    return Status::ok();
}

void OtDaemonServer::runOtCtlCommandInternal(const std::string                        &aCommand,
                                             const bool                                aIsInteractive,
                                             const std::shared_ptr<IOtOutputReceiver> &aReceiver)
{
    otSysCliInitUsingDaemon(GetOtInstance());

    if (!aCommand.empty())
    {
        std::string command = aCommand;

        mIsOtCtlInteractiveMode = aIsInteractive;
        mOtCtlOutputReceiver    = aReceiver;

        otCliInit(GetOtInstance(), OtDaemonServer::OtCtlCommandCallback, this);
        otCliInputLine(command.data());
    }
}

void OtDaemonServer::setInfraLinkNat64PrefixInternal(const std::string                        &aNat64Prefix,
                                                     const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    otError     error = OT_ERROR_NONE;
    std::string message;

    otbrLogInfo("Setting infra link NAT64 prefix: %s", aNat64Prefix.c_str());

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    mInfraLinkState.nat64Prefix = aNat64Prefix;
    NotifyNat64PrefixDiscoveryDone();

exit:
    PropagateResult(error, message, aReceiver);
}

std::vector<otIp6Address> ToOtUpstreamDnsServerAddresses(const std::vector<std::string> &aAddresses)
{
    std::vector<otIp6Address> addresses;

    // TODO: b/363738575 - support IPv6
    for (const auto &addressString : aAddresses)
    {
        otIp6Address ip6Address;
        otIp4Address ip4Address;

        if (otIp4AddressFromString(addressString.c_str(), &ip4Address) != OT_ERROR_NONE)
        {
            continue;
        }
        otIp4ToIp4MappedIp6Address(&ip4Address, &ip6Address);
        addresses.push_back(ip6Address);
    }

    return addresses;
}

Status OtDaemonServer::setInfraLinkDnsServers(const std::vector<std::string>           &aDnsServers,
                                              const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post([aDnsServers, aReceiver, this]() { setInfraLinkDnsServersInternal(aDnsServers, aReceiver); });

    return Status::ok();
}

void OtDaemonServer::setInfraLinkDnsServersInternal(const std::vector<std::string>           &aDnsServers,
                                                    const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    otError     error = OT_ERROR_NONE;
    std::string message;
    auto        dnsServers = ToOtUpstreamDnsServerAddresses(aDnsServers);

    otbrLogInfo("Setting infra link DNS servers: %d servers", aDnsServers.size());

    VerifyOrExit(aDnsServers != mInfraLinkState.dnsServers);

    mInfraLinkState.dnsServers = aDnsServers;
    otSysUpstreamDnsSetServerList(dnsServers.data(), dnsServers.size());

exit:
    PropagateResult(error, message, aReceiver);
}

otLinkModeConfig OtDaemonServer::GetLinkModeConfig(bool aIsRouter)
{
    otLinkModeConfig linkModeConfig{};

    if (aIsRouter)
    {
        linkModeConfig.mRxOnWhenIdle = true;
        linkModeConfig.mDeviceType   = true;
        linkModeConfig.mNetworkData  = true;
    }
    else
    {
        linkModeConfig.mRxOnWhenIdle = false;
        linkModeConfig.mDeviceType   = false;
        linkModeConfig.mNetworkData  = true;
    }

    return linkModeConfig;
}

static int OutputCallback(void *aContext, const char *aFormat, va_list aArguments)
{
    std::string output;

    android::base::StringAppendV(&output, aFormat, aArguments);

    int length = output.length();

    VerifyOrExit(android::base::WriteStringToFd(output, *(static_cast<int *>(aContext))), length = 0);

exit:
    return length;
}

inline void DumpCliCommand(std::string aCommand, int aFd)
{
    android::base::WriteStringToFd(aCommand + '\n', aFd);
    otCliInputLine(aCommand.data());
}

binder_status_t OtDaemonServer::dump(int aFd, const char **aArgs, uint32_t aNumArgs)
{
    OT_UNUSED_VARIABLE(aArgs);
    OT_UNUSED_VARIABLE(aNumArgs);

    otCliInit(GetOtInstance(), OutputCallback, &aFd);

    DumpCliCommand("state", aFd);
    DumpCliCommand("srp server state", aFd);
    DumpCliCommand("srp server service", aFd);
    DumpCliCommand("srp server host", aFd);
    DumpCliCommand("dataset activetimestamp", aFd);
    DumpCliCommand("dataset channel", aFd);
    DumpCliCommand("dataset channelmask", aFd);
    DumpCliCommand("dataset extpanid", aFd);
    DumpCliCommand("dataset meshlocalprefix", aFd);
    DumpCliCommand("dataset networkname", aFd);
    DumpCliCommand("dataset panid", aFd);
    DumpCliCommand("dataset securitypolicy", aFd);
    DumpCliCommand("leaderdata", aFd);
    DumpCliCommand("eidcache", aFd);
    DumpCliCommand("counters mac", aFd);
    DumpCliCommand("counters mle", aFd);
    DumpCliCommand("counters ip", aFd);
    DumpCliCommand("router table", aFd);
    DumpCliCommand("neighbor table", aFd);
    DumpCliCommand("ipaddr -v", aFd);
    DumpCliCommand("netdata show", aFd);

    fsync(aFd);

    otSysCliInitUsingDaemon(GetOtInstance());

    return STATUS_OK;
}

void OtDaemonServer::PushTelemetryIfConditionMatch()
{
    VerifyOrExit(GetOtInstance() != nullptr);

    // TODO: Push telemetry per kTelemetryUploadIntervalThreshold instead of on startup.
    // TODO: Save unpushed telemetries in local cache to avoid data loss.
    RetrieveAndPushAtoms(GetOtInstance());
    mTaskRunner.Post(kTelemetryUploadIntervalThreshold, [this]() { PushTelemetryIfConditionMatch(); });

exit:
    return;
}

void OtDaemonServer::NotifyNat64PrefixDiscoveryDone(void)
{
    otIp6Prefix nat64Prefix{};
    uint32_t    infraIfIndex = if_nametoindex(mInfraLinkState.interfaceName.value_or("").c_str());

    otIp6PrefixFromString(mInfraLinkState.nat64Prefix.value_or("").c_str(), &nat64Prefix);
    otPlatInfraIfDiscoverNat64PrefixDone(GetOtInstance(), infraIfIndex, &nat64Prefix);

exit:
    return;
}

extern "C" otError otPlatInfraIfDiscoverNat64Prefix(uint32_t aInfraIfIndex)
{
    OT_UNUSED_VARIABLE(aInfraIfIndex);

    OtDaemonServer *otDaemonServer = OtDaemonServer::Get();
    otError         error          = OT_ERROR_NONE;

    VerifyOrExit(otDaemonServer != nullptr, error = OT_ERROR_INVALID_STATE);

    otDaemonServer->NotifyNat64PrefixDiscoveryDone();

exit:
    return error;
}

Status OtDaemonServer::setNat64Cidr(const std::optional<std::string>         &aCidr,
                                    const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post([aCidr, aReceiver, this]() { setNat64CidrInternal(aCidr, aReceiver); });

    return Status::ok();
}

void OtDaemonServer::setNat64CidrInternal(const std::optional<std::string>         &aCidr,
                                          const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    otError     error = OT_ERROR_NONE;
    std::string message;

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    if (aCidr.has_value())
    {
        otIp4Cidr nat64Cidr{};

        otbrLogInfo("Setting NAT64 CIDR: %s", aCidr->c_str());
        SuccessOrExit(error = otIp4CidrFromString(aCidr->c_str(), &nat64Cidr), message = "Failed to parse NAT64 CIDR");
        SuccessOrExit(error = otNat64SetIp4Cidr(GetOtInstance(), &nat64Cidr), message = "Failed to set NAT64 CIDR");
    }
    else
    {
        otbrLogInfo("Clearing NAT64 CIDR");
        otNat64ClearIp4Cidr(GetOtInstance());
    }

exit:
    PropagateResult(error, message, aReceiver);
}

} // namespace Android
} // namespace otbr
