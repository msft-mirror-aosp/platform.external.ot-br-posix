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

#define OTBR_LOG_TAG "BINDER"

#include "android/otdaemon_server.hpp"

#include <net/if.h>
#include <string.h>

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <openthread/border_router.h>
#include <openthread/cli.h>
#include <openthread/icmp6.h>
#include <openthread/ip6.h>
#include <openthread/link.h>
#include <openthread/openthread-system.h>
#include <openthread/platform/infra_if.h>
#include <openthread/platform/radio.h>

#include "agent/vendor.hpp"
#include "android/otdaemon_telemetry.hpp"
#include "common/code_utils.hpp"

#define BYTE_ARR_END(arr) ((arr) + sizeof(arr))

namespace otbr {

namespace vendor {

std::shared_ptr<VendorServer> VendorServer::newInstance(Application &aApplication)
{
    return ndk::SharedRefBase::make<Android::OtDaemonServer>(aApplication);
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

OtDaemonServer::OtDaemonServer(Application &aApplication)
    : mApplication(aApplication)
    , mNcp(aApplication.GetNcp())
    , mBorderAgent(aApplication.GetBorderAgent())
    , mMdnsPublisher(static_cast<MdnsPublisher &>(aApplication.GetPublisher()))
    , mBorderRouterConfiguration()
{
    mClientDeathRecipient =
        ::ndk::ScopedAIBinder_DeathRecipient(AIBinder_DeathRecipient_new(&OtDaemonServer::BinderDeathCallback));
    mBorderRouterConfiguration.infraInterfaceName        = "";
    mBorderRouterConfiguration.infraInterfaceIcmp6Socket = ScopedFileDescriptor();
    mBorderRouterConfiguration.isBorderRoutingEnabled    = false;
}

void OtDaemonServer::Init(void)
{
    binder_exception_t exp = AServiceManager_registerLazyService(asBinder().get(), OTBR_SERVICE_NAME);
    SuccessOrDie(exp, "Failed to register OT daemon binder service");

    assert(GetOtInstance() != nullptr);

    mNcp.AddThreadStateChangedCallback([this](otChangedFlags aFlags) { StateCallback(aFlags); });
    otIp6SetAddressCallback(GetOtInstance(), OtDaemonServer::AddressCallback, this);
    otIp6SetReceiveCallback(GetOtInstance(), OtDaemonServer::ReceiveCallback, this);
    otBackboneRouterSetMulticastListenerCallback(GetOtInstance(), OtDaemonServer::HandleBackboneMulticastListenerEvent,
                                                 this);
    otIcmp6SetEchoMode(GetOtInstance(), OT_ICMP6_ECHO_HANDLER_DISABLED);

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
    assert(GetOtInstance() != nullptr);

    if (RefreshOtDaemonState(aFlags))
    {
        if (mCallback == nullptr)
        {
            otbrLogWarning("Ignoring OT state changes: callback is not set");
        }
        else
        {
            mCallback->onStateChanged(mState, -1);
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

// FIXME(wgtdkp): We should reuse the same code in openthread/src/posix/platform/netif.cp
// after the refactor there is done: https://github.com/openthread/openthread/pull/9293
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

// FIXME(wgtdkp): this doesn't support NAT64, we should use a shared library with ot-posix
// to handle packet translations between the tunnel interface and Thread.
void OtDaemonServer::TransmitCallback(void)
{
    char              packet[kMaxIp6Size];
    ssize_t           length;
    otMessage        *message = nullptr;
    otError           error   = OT_ERROR_NONE;
    otMessageSettings settings;
    int               fd = mTunFd.get();

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

    message = otIp6NewMessage(GetOtInstance(), &settings);
    VerifyOrExit(message != nullptr, error = OT_ERROR_NO_BUFS);
    otMessageSetOrigin(message, OT_MESSAGE_ORIGIN_HOST_UNTRUSTED);

    SuccessOrExit(error = otMessageAppend(message, packet, static_cast<uint16_t>(length)));

    error   = otIp6Send(GetOtInstance(), message);
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
    otbrLogInfo("Updating backbone router state (bbr state = %d)", bbrState);

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

    otbrLogDebug("Multicast forwarding address changed, %s is %s", addressString,
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
    return mNcp.GetInstance();
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
                                  const bool                                enabled,
                                  const std::shared_ptr<INsdPublisher>     &aINsdPublisher,
                                  const MeshcopTxtAttributes               &aMeshcopTxts,
                                  const std::shared_ptr<IOtDaemonCallback> &aCallback,
                                  const std::string                        &aCountryCode)
{
    otbrLogInfo("OT daemon is initialized by system server (tunFd=%d, enabled=%s)", aTunFd.get(),
                enabled ? "true" : "false");
    // The copy constructor of `ScopedFileDescriptor` is deleted. It is unable to pass the `aTunFd`
    // to the lambda function. The processing method of `aTunFd` doesn't call OpenThread functions,
    // we can process `aTunFd` directly in front of the task.
    mTunFd = aTunFd.dup();

    mINsdPublisher = aINsdPublisher;
    mMeshcopTxts   = aMeshcopTxts;

    mTaskRunner.Post([enabled, aINsdPublisher, aMeshcopTxts, aCallback, aCountryCode, this]() {
        initializeInternal(enabled, mINsdPublisher, mMeshcopTxts, aCallback, aCountryCode);
    });

    return Status::ok();
}

void OtDaemonServer::initializeInternal(const bool                                enabled,
                                        const std::shared_ptr<INsdPublisher>     &aINsdPublisher,
                                        const MeshcopTxtAttributes               &aMeshcopTxts,
                                        const std::shared_ptr<IOtDaemonCallback> &aCallback,
                                        const std::string                        &aCountryCode)
{
    std::string instanceName = aMeshcopTxts.vendorName + " " + aMeshcopTxts.modelName;

    setCountryCodeInternal(aCountryCode, nullptr /* aReceiver */);
    registerStateCallbackInternal(aCallback, -1 /* listenerId */);

    mMdnsPublisher.SetINsdPublisher(aINsdPublisher);
    mBorderAgent.SetMeshCopServiceValues(instanceName, aMeshcopTxts.modelName, aMeshcopTxts.vendorName,
                                         aMeshcopTxts.vendorOui);
    mBorderAgent.SetEnabled(enabled);

    if (enabled)
    {
        enableThread(nullptr /* aReceiver */);
    }
    else
    {
        updateThreadEnabledState(OT_STATE_DISABLED, nullptr /* aReceiver */);
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

void OtDaemonServer::updateThreadEnabledState(const int enabled, const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    VerifyOrExit(enabled != mThreadEnabled);

    otbrLogInfo("Thread enabled state changed: %s -> %s", ThreadEnabledStateToString(mThreadEnabled),
                ThreadEnabledStateToString(enabled));
    mThreadEnabled = enabled;

    if (aReceiver != nullptr)
    {
        aReceiver->onSuccess();
    }

    // Enables the BorderAgent module only when Thread is enabled because it always
    // publishes the MeshCoP service even when no Thread network is provisioned.
    switch (enabled)
    {
    case OT_STATE_ENABLED:
        mBorderAgent.SetEnabled(true);
        break;
    case OT_STATE_DISABLED:
        mBorderAgent.SetEnabled(false);
        break;
    }

    if (mCallback != nullptr)
    {
        mCallback->onThreadEnabledChanged(mThreadEnabled);
    }

exit:
    return;
}

void OtDaemonServer::enableThread(const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    otOperationalDatasetTlvs datasetTlvs;

    if (otDatasetGetActiveTlvs(GetOtInstance(), &datasetTlvs) != OT_ERROR_NOT_FOUND && datasetTlvs.mLength > 0 &&
        !isAttached())
    {
        (void)otIp6SetEnabled(GetOtInstance(), true);
        (void)otThreadSetEnabled(GetOtInstance(), true);
    }
    updateThreadEnabledState(OT_STATE_ENABLED, aReceiver);
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

    VerifyOrExit(mThreadEnabled != OT_STATE_DISABLING, error = OT_ERROR_BUSY, message = "Thread is disabling");

    if ((mThreadEnabled == OT_STATE_ENABLED) == enabled)
    {
        aReceiver->onSuccess();
        ExitNow();
    }

    if (enabled)
    {
        enableThread(aReceiver);
    }
    else
    {
        // `aReceiver` should not be set here because the operation isn't finished yet
        updateThreadEnabledState(OT_STATE_DISABLING, nullptr /* aReceiver */);

        LeaveGracefully([aReceiver, this]() {
            // Ignore errors as those operations should always succeed
            (void)otThreadSetEnabled(GetOtInstance(), false);
            (void)otIp6SetEnabled(GetOtInstance(), false);
            updateThreadEnabledState(OT_STATE_DISABLED, aReceiver);
        });
    }

exit:
    if (error != OT_ERROR_NONE)
    {
        PropagateResult(error, message, aReceiver);
    }
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
    mCallback->onStateChanged(mState, listenerId);
    mCallback->onThreadEnabledChanged(mThreadEnabled);
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
    otOperationalDatasetTlvs datasetTlvs;

    VerifyOrExit(mThreadEnabled != OT_STATE_DISABLING, error = OT_ERROR_BUSY, message = "Thread is disabling");

    VerifyOrExit(mThreadEnabled == OT_STATE_ENABLED,
                 error   = static_cast<int>(IOtDaemon::ErrorCode::OT_ERROR_THREAD_DISABLED),
                 message = "Thread is disabled");

    otbrLogInfo("Start joining...");

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    if (otThreadGetDeviceRole(GetOtInstance()) != OT_DEVICE_ROLE_DISABLED)
    {
        LeaveGracefully([aActiveOpDatasetTlvs, aReceiver, this]() {
            FinishLeave(nullptr);
            join(aActiveOpDatasetTlvs, aReceiver);
        });
        ExitNow();
    }

    std::copy(aActiveOpDatasetTlvs.begin(), aActiveOpDatasetTlvs.end(), datasetTlvs.mTlvs);
    datasetTlvs.mLength = static_cast<uint8_t>(aActiveOpDatasetTlvs.size());
    SuccessOrExit(error   = otDatasetSetActiveTlvs(GetOtInstance(), &datasetTlvs),
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

Status OtDaemonServer::leave(const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    mTaskRunner.Post([aReceiver, this]() { leaveInternal(aReceiver); });

    return Status::ok();
}

void OtDaemonServer::leaveInternal(const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    std::string message;
    int         error = OT_ERROR_NONE;

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    VerifyOrExit(mThreadEnabled != OT_STATE_DISABLING, error = OT_ERROR_BUSY, message = "Thread is disabling");

    if (mThreadEnabled == OT_STATE_DISABLED)
    {
        FinishLeave(aReceiver);
        ExitNow();
    }

    LeaveGracefully([aReceiver, this]() { FinishLeave(aReceiver); });

exit:
    if (error != OT_ERROR_NONE)
    {
        PropagateResult(error, message, aReceiver);
    }
}

void OtDaemonServer::FinishLeave(const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    (void)otInstanceErasePersistentInfo(GetOtInstance());
    OT_UNUSED_VARIABLE(mApplication); // Avoid the unused-private-field issue.
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

    VerifyOrExit(mThreadEnabled != OT_STATE_DISABLING, error = OT_ERROR_BUSY, message = "Thread is disabling");

    VerifyOrExit(mThreadEnabled == OT_STATE_ENABLED,
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
    static constexpr int kCountryCodeLength = 2;
    otError              error              = OT_ERROR_NONE;
    std::string          message;
    uint16_t             countryCode;

    VerifyOrExit((aCountryCode.length() == kCountryCodeLength) && isalpha(aCountryCode[0]) && isalpha(aCountryCode[1]),
                 error = OT_ERROR_INVALID_ARGS, message = "The country code is invalid");

    otbrLogInfo("Set country code: %c%c", aCountryCode[0], aCountryCode[1]);
    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    countryCode = static_cast<uint16_t>((aCountryCode[0] << 8) | aCountryCode[1]);
    SuccessOrExit(error = otLinkSetRegion(GetOtInstance(), countryCode), message = "Failed to set the country code");

exit:
    PropagateResult(error, message, aReceiver);
}

Status OtDaemonServer::getChannelMasks(const std::shared_ptr<IChannelMasksReceiver> &aReceiver)
{
    mTaskRunner.Post([aReceiver, this]() { getChannelMasksInternal(aReceiver); });

    return Status::ok();
}

void OtDaemonServer::getChannelMasksInternal(const std::shared_ptr<IChannelMasksReceiver> &aReceiver)
{
    otError  error = OT_ERROR_NONE;
    uint32_t supportedChannelMask;
    uint32_t preferredChannelMask;

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE);

    supportedChannelMask = otLinkGetSupportedChannelMask(GetOtInstance());
    preferredChannelMask = otPlatRadioGetPreferredChannelMask(GetOtInstance());

exit:
    if (aReceiver != nullptr)
    {
        if (error == OT_ERROR_NONE)
        {
            aReceiver->onSuccess(supportedChannelMask, preferredChannelMask);
        }
        else
        {
            aReceiver->onError(error, "OT is not initialized");
        }
    }
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
    otError     error = OT_ERROR_NONE;
    std::string message;
    uint8_t     channel;
    int16_t     maxPower;

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    for (ChannelMaxPower channelMaxPower : aChannelMaxPowers)
    {
        VerifyOrExit((channelMaxPower.channel >= OT_RADIO_2P4GHZ_OQPSK_CHANNEL_MIN) &&
                         (channelMaxPower.channel <= OT_RADIO_2P4GHZ_OQPSK_CHANNEL_MAX),
                     error = OT_ERROR_INVALID_ARGS, message = "The channel is invalid");
        VerifyOrExit((channelMaxPower.maxPower >= INT16_MIN) && (channelMaxPower.maxPower <= INT16_MAX),
                     error = OT_ERROR_INVALID_ARGS, message = "The max power is invalid");
    }

    for (ChannelMaxPower channelMaxPower : aChannelMaxPowers)
    {
        channel  = static_cast<uint8_t>(channelMaxPower.channel);
        maxPower = static_cast<int16_t>(channelMaxPower.maxPower);
        otbrLogInfo("Set channel max power: channel=%u, maxPower=%d", channel, maxPower);
        SuccessOrExit(error   = otPlatRadioSetChannelTargetPower(GetOtInstance(), channel, maxPower),
                      message = "Failed to set channel max power");
    }

exit:
    PropagateResult(error, message, aReceiver);
    return Status::ok();
}

Status OtDaemonServer::configureBorderRouter(const BorderRouterConfigurationParcel    &aBorderRouterConfiguration,
                                             const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    int         icmp6SocketFd               = aBorderRouterConfiguration.infraInterfaceIcmp6Socket.dup().release();
    std::string infraInterfaceName          = aBorderRouterConfiguration.infraInterfaceName;
    bool        isBorderRoutingEnabled      = aBorderRouterConfiguration.isBorderRoutingEnabled;
    bool        isBorderRouterConfigChanged = (mBorderRouterConfiguration != aBorderRouterConfiguration);

    otbrLogInfo("Configuring Border Router: %s", aBorderRouterConfiguration.toString().c_str());

    // The copy constructor of `BorderRouterConfigurationParcel` is deleted. It is unable to directly pass the
    // `aBorderRouterConfiguration` to the lambda function. Only the necessary parameters of
    // `BorderRouterConfigurationParcel` are passed to the lambda function here.
    mTaskRunner.Post(
        [icmp6SocketFd, infraInterfaceName, isBorderRoutingEnabled, isBorderRouterConfigChanged, aReceiver, this]() {
            configureBorderRouterInternal(icmp6SocketFd, infraInterfaceName, isBorderRoutingEnabled,
                                          isBorderRouterConfigChanged, aReceiver);
        });

    return Status::ok();
}

void OtDaemonServer::configureBorderRouterInternal(int                aIcmp6SocketFd,
                                                   const std::string &aInfraInterfaceName,
                                                   bool               aIsBorderRoutingEnabled,
                                                   bool               aIsBorderRouterConfigChanged,
                                                   const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    int         icmp6SocketFd = aIcmp6SocketFd;
    otError     error         = OT_ERROR_NONE;
    std::string message;

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    if (aIsBorderRouterConfigChanged)
    {
        if (aIsBorderRoutingEnabled)
        {
            unsigned int infraIfIndex = if_nametoindex(aInfraInterfaceName.c_str());
            SuccessOrExit(error   = otBorderRoutingSetEnabled(GetOtInstance(), false /* aEnabled */),
                          message = "failed to disable border routing");
            otSysSetInfraNetif(aInfraInterfaceName.c_str(), icmp6SocketFd);
            icmp6SocketFd = -1;
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
    }

    mBorderRouterConfiguration.isBorderRoutingEnabled = aIsBorderRoutingEnabled;
    mBorderRouterConfiguration.infraInterfaceName     = aInfraInterfaceName;

exit:
    if (error != OT_ERROR_NONE)
    {
        close(icmp6SocketFd);
    }
    PropagateResult(error, message, aReceiver);
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
    DumpCliCommand("dataset active", aFd);
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

} // namespace Android
} // namespace otbr
