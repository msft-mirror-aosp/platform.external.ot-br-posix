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
#include <openthread/instance.h>
#include <openthread/ip6.h>

#include "agent/application.hpp"
#include "agent/vendor.hpp"
#include "common/mainloop.hpp"
#include "ncp/ncp_openthread.hpp"

namespace otbr {
namespace Android {

using BinderDeathRecipient = ::ndk::ScopedAIBinder_DeathRecipient;
using ScopedFileDescriptor = ::ndk::ScopedFileDescriptor;
using Status               = ::ndk::ScopedAStatus;
using aidl::com::android::server::thread::openthread::BnOtDaemon;
using aidl::com::android::server::thread::openthread::IOtDaemonCallback;
using aidl::com::android::server::thread::openthread::IOtStatusReceiver;
using aidl::com::android::server::thread::openthread::Ipv6AddressInfo;

class OtDaemonServer : public BnOtDaemon, public MainloopProcessor, public vendor::VendorServer
{
public:
    explicit OtDaemonServer(Application &aApplication);
    virtual ~OtDaemonServer(void) = default;

    // Disallow copy and assign.
    OtDaemonServer(const OtDaemonServer &) = delete;
    void operator=(const OtDaemonServer &) = delete;

    // Dump information for debugging.
    binder_status_t dump(int aFd, const char **aArgs, uint32_t aNumArgs) override;

private:
    using DetachCallback = std::function<void()>;

    otInstance *GetOtInstance(void);

    // Implements vendor::VendorServer

    void Init(void) override;

    // Implements MainloopProcessor

    void Update(MainloopContext &aMainloop) override;
    void Process(const MainloopContext &aMainloop) override;

    // Implements IOtDaemon.aidl

    Status initialize(const ScopedFileDescriptor &aTunFd, const std::shared_ptr<IOtDaemonCallback> &aCallback) override;
    Status getExtendedMacAddress(std::vector<uint8_t> *aExtendedMacAddress) override;
    Status getThreadVersion(int *aThreadVersion) override;
    bool   isAttached(void);
    Status attach(bool                                      aDoForm,
                  const std::vector<uint8_t>               &aActiveOpDatasetTlvs,
                  const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    Status detach(const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void   detachGracefully(const DetachCallback &aCallback);
    Status scheduleMigration(const std::vector<uint8_t>               &aPendingOpDatasetTlvs,
                             const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    static void sendMgmtPendingSetCallback(otError aResult, void *aBinderServer);

    static void BinderDeathCallback(void *aBinderServer);
    void        StateCallback(otChangedFlags aFlags);
    static void AddressCallback(const otIp6AddressInfo *aAddressInfo, bool aIsAdded, void *aBinderServer);
    static void ReceiveCallback(otMessage *aMessage, void *aBinderServer);
    void        ReceiveCallback(otMessage *aMessage);
    void        TransmitCallback(void);
    static void DetachGracefullyCallback(void *aBinderServer);

    otbr::Ncp::ControllerOpenThread   &mNcp;
    ScopedFileDescriptor               mTunFd;
    std::shared_ptr<IOtDaemonCallback> mCallback;
    BinderDeathRecipient               mClientDeathRecipient;
    std::vector<DetachCallback>        mOngoingDetachCallbacks;
};

} // namespace Android
} // namespace otbr

#endif // OTBR_ANDROID_BINDER_SERVER_HPP_
