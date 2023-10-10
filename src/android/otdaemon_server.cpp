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
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <openthread/border_router.h>
#include <openthread/ip6.h>
#include <openthread/openthread-system.h>
#include <openthread/platform/infra_if.h>

#include "agent/vendor.hpp"
#include "common/code_utils.hpp"

#define BYTE_ARR_END(arr) ((arr) + sizeof(arr))

namespace otbr {

namespace vendor {

std::shared_ptr<VendorServer> VendorServer::newInstance(Application &aApplication)
{
    return ndk::SharedRefBase::make<Android::OtDaemonServer>(aApplication.GetNcp());
}

} // namespace vendor

} // namespace otbr

namespace otbr {
namespace Android {

static const char       OTBR_SERVICE_NAME[] = "ot_daemon";
static constexpr size_t kMaxIp6Size         = 1280;

static void PropagateResult(otError                                   aError,
                            const std::string                        &aMessage,
                            const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    if (aReceiver != nullptr)
    {
        (aError == OT_ERROR_NONE) ? aReceiver->onSuccess() : aReceiver->onError(aError, aMessage);
    }
}

static Ipv6AddressInfo ConvertToAddressInfo(const otIp6AddressInfo &aAddressInfo)
{
    Ipv6AddressInfo addrInfo;

    addrInfo.address.assign(aAddressInfo.mAddress->mFields.m8, BYTE_ARR_END(aAddressInfo.mAddress->mFields.m8));
    addrInfo.prefixLength = aAddressInfo.mPrefixLength;
    addrInfo.scope        = aAddressInfo.mScope;
    addrInfo.isPreferred  = aAddressInfo.mPreferred;
    return addrInfo;
}

OtDaemonServer::OtDaemonServer(otbr::Ncp::ControllerOpenThread &aNcp)
    : mNcp(aNcp)
{
    mClientDeathRecipient =
        ::ndk::ScopedAIBinder_DeathRecipient(AIBinder_DeathRecipient_new(&OtDaemonServer::BinderDeathCallback));
}

void OtDaemonServer::Init(void)
{
    binder_exception_t exp = AServiceManager_registerLazyService(asBinder().get(), OTBR_SERVICE_NAME);
    SuccessOrDie(exp, "Failed to register OT daemon binder service");

    assert(GetOtInstance() != nullptr);

    mNcp.AddThreadStateChangedCallback([this](otChangedFlags aFlags) { StateCallback(aFlags); });
    otIp6SetAddressCallback(GetOtInstance(), OtDaemonServer::AddressCallback, this);
    otIp6SetReceiveCallback(GetOtInstance(), OtDaemonServer::ReceiveCallback, this);
}

void OtDaemonServer::BinderDeathCallback(void *aBinderServer)
{
    OtDaemonServer *thisServer = static_cast<OtDaemonServer *>(aBinderServer);

    otbrLogCrit("Client is died, removing callbacks...");
    thisServer->mCallback = nullptr;
    thisServer->mTunFd.set(-1); // the original FD will be closed automatically
}

void OtDaemonServer::StateCallback(otChangedFlags aFlags)
{
    assert(GetOtInstance() != nullptr);

    if (mCallback == nullptr)
    {
        otbrLogWarning("Ignoring OT state changes: callback is not set");
        ExitNow();
    }

    if (aFlags & OT_CHANGED_THREAD_NETIF_STATE)
    {
        mCallback->onInterfaceStateChanged(otIp6IsEnabled(GetOtInstance()));
    }

    if (aFlags & OT_CHANGED_THREAD_ROLE)
    {
        mCallback->onDeviceRoleChanged(otThreadGetDeviceRole(GetOtInstance()));

        if (!isAttached())
        {
            for (const auto &detachCallback : mOngoingDetachCallbacks)
            {
                detachCallback();
            }
            mOngoingDetachCallbacks.clear();
        }
    }

    if (aFlags & OT_CHANGED_THREAD_PARTITION_ID)
    {
        mCallback->onPartitionIdChanged(otThreadGetPartitionId(GetOtInstance()));
    }

    if (aFlags & OT_CHANGED_ACTIVE_DATASET)
    {
        std::vector<uint8_t>     result;
        otOperationalDatasetTlvs datasetTlvs;
        if (otDatasetGetActiveTlvs(GetOtInstance(), &datasetTlvs) == OT_ERROR_NONE)
        {
            result.assign(datasetTlvs.mTlvs, datasetTlvs.mTlvs + datasetTlvs.mLength);
        }
        mCallback->onActiveOperationalDatasetChanged(result);
    }

    if (aFlags & OT_CHANGED_PENDING_DATASET)
    {
        std::vector<uint8_t>     result;
        otOperationalDatasetTlvs datasetTlvs;
        if (otDatasetGetPendingTlvs(GetOtInstance(), &datasetTlvs) == OT_ERROR_NONE)
        {
            result.assign(datasetTlvs.mTlvs, datasetTlvs.mTlvs + datasetTlvs.mLength);
        }
        mCallback->onPendingOperationalDatasetChanged(result);
    }

exit:
    return;
}

void OtDaemonServer::AddressCallback(const otIp6AddressInfo *aAddressInfo, bool aIsAdded, void *aBinderServer)
{
    OtDaemonServer *thisServer = static_cast<OtDaemonServer *>(aBinderServer);

    if (thisServer->mCallback != nullptr)
    {
        thisServer->mCallback->onAddressChanged(ConvertToAddressInfo(*aAddressInfo), aIsAdded);
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

    SuccessOrExit(error = otMessageAppend(message, packet, length));

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
                                  const std::shared_ptr<IOtDaemonCallback> &aCallback)
{
    otbrLogDebug("OT daemon is initialized by the binder client (tunFd=%d)", aTunFd.get());

    mTunFd    = aTunFd.dup();
    mCallback = aCallback;
    if (mCallback != nullptr)
    {
        AIBinder_linkToDeath(mCallback->asBinder().get(), mClientDeathRecipient.get(), this);
    }

    return Status::ok();
}

Status OtDaemonServer::attach(bool                                      aDoForm,
                              const std::vector<uint8_t>               &aActiveOpDatasetTlvs,
                              const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    otError                  error = OT_ERROR_NONE;
    std::string              message;
    otOperationalDatasetTlvs datasetTlvs;

    // TODO(b/273160198): check how we can implement attach-only behavior
    (void)aDoForm;

    otbrLogInfo("Start attaching...");

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");
    VerifyOrExit(!isAttached(), error = OT_ERROR_INVALID_STATE, message = "Cannot attach when already attached");

    std::copy(aActiveOpDatasetTlvs.begin(), aActiveOpDatasetTlvs.end(), datasetTlvs.mTlvs);
    datasetTlvs.mLength = aActiveOpDatasetTlvs.size();
    SuccessOrExit(error   = otDatasetSetActiveTlvs(GetOtInstance(), &datasetTlvs),
                  message = "Failed to set Active Operational Dataset");

    // Shouldn't we have an equivalent `otThreadAttach` method vs `otThreadDetachGracefully`?
    SuccessOrExit(error = otIp6SetEnabled(GetOtInstance(), true), message = "Failed to bring up Thread interface");
    SuccessOrExit(error = otThreadSetEnabled(GetOtInstance(), true), message = "Failed to bring up Thread stack");

exit:
    PropagateResult(error, message, aReceiver);
    return Status::ok();
}

void OtDaemonServer::detachGracefully(const DetachCallback &aCallback)
{
    otError error;

    mOngoingDetachCallbacks.push_back(aCallback);

    // The callback is already guarded by a timer inside OT, so the client side shouldn't need to
    // add a callback again.
    error = otThreadDetachGracefully(GetOtInstance(), OtDaemonServer::DetachGracefullyCallback, this);
    if (error == OT_ERROR_BUSY)
    {
        // There is already an ongoing detach request, do nothing but enqueue the callback
        otbrLogDebug("Reuse existing detach() request");
        ExitNow(error = OT_ERROR_NONE);
    }

exit:;
}

Status OtDaemonServer::detach(const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    if (GetOtInstance() == nullptr)
    {
        PropagateResult(OT_ERROR_INVALID_STATE, "OT is not initialized", aReceiver);
    }
    else
    {
        detachGracefully([=]() {
            if (aReceiver != nullptr)
            {
                aReceiver->onSuccess();
            }
        });
    }

    return Status::ok();
}

void OtDaemonServer::DetachGracefullyCallback(void *aBinderServer)
{
    OtDaemonServer *thisServer = static_cast<OtDaemonServer *>(aBinderServer);

    for (auto callback : thisServer->mOngoingDetachCallbacks)
    {
        callback();
    }
    thisServer->mOngoingDetachCallbacks.clear();
}

bool OtDaemonServer::isAttached()
{
    otDeviceRole role = otThreadGetDeviceRole(GetOtInstance());

    return role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
}

Status OtDaemonServer::scheduleMigration(const std::vector<uint8_t>               &aPendingOpDatasetTlvs,
                                         const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    otError              error = OT_ERROR_NONE;
    std::string          message;
    otOperationalDataset emptyDataset;

    if (GetOtInstance() == nullptr)
    {
        message = "OT is not initialized";
        ExitNow(error = OT_ERROR_INVALID_STATE);
    }

    if (!isAttached())
    {
        message = "Cannot schedule migration when this device is detached";
        ExitNow(error = OT_ERROR_INVALID_STATE);
    }

    error = otDatasetSendMgmtPendingSet(GetOtInstance(), &emptyDataset, aPendingOpDatasetTlvs.data(),
                                        aPendingOpDatasetTlvs.size(), sendMgmtPendingSetCallback,
                                        /* aBinderServer= */ this);
    if (error != OT_ERROR_NONE)
    {
        message = "Failed to send MGMT_PENDING_SET.req";
    }

exit:
    PropagateResult(error, message, aReceiver);
    return Status::ok();
}

void OtDaemonServer::sendMgmtPendingSetCallback(otError aResult, void *aBinderServer)
{
    (void)aBinderServer;

    otbrLogDebug("otDatasetSendMgmtPendingSet callback: %d", aResult);
}

Status OtDaemonServer::getExtendedMacAddress(std::vector<uint8_t> *aExtendedMacAddress)
{
    Status              status = Status::ok();
    const otExtAddress *extAddress;

    if (aExtendedMacAddress == nullptr)
    {
        status =
            Status::fromServiceSpecificErrorWithMessage(OT_ERROR_INVALID_ARGS, "aExtendedMacAddress can not be null");
        ExitNow();
    }

    if (GetOtInstance() == nullptr)
    {
        status = Status::fromServiceSpecificErrorWithMessage(OT_ERROR_INVALID_STATE, "OT is not initialized");
        ExitNow();
    }

    extAddress = otLinkGetExtendedAddress(GetOtInstance());
    aExtendedMacAddress->assign(extAddress->m8, extAddress->m8 + sizeof(extAddress->m8));

exit:
    return status;
}

Status OtDaemonServer::getThreadVersion(int *aThreadVersion)
{
    Status status = Status::ok();

    if (aThreadVersion == nullptr)
    {
        status = Status::fromServiceSpecificErrorWithMessage(OT_ERROR_INVALID_ARGS, "aThreadVersion can not be null");
        ExitNow();
    }

    *aThreadVersion = otThreadGetVersion();

exit:
    return status;
}

binder_status_t OtDaemonServer::dump(int aFd, const char** aArgs, uint32_t aNumArgs)
{
    OT_UNUSED_VARIABLE(aArgs);
    OT_UNUSED_VARIABLE(aNumArgs);

    // TODO: Use ::android::base::WriteStringToFd to dump infomration.
    fsync(aFd);

    return STATUS_OK;
}
} // namespace Android
} // namespace otbr
