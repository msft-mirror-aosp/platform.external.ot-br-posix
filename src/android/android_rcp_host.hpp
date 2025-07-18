/*
 *  Copyright (c) 2024, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef OTBR_ANDROID_RCP_HOST_HPP_
#define OTBR_ANDROID_RCP_HOST_HPP_

#include "android_thread_host.hpp"

#include <memory>

#include "common_utils.hpp"
#include "host/rcp_host.hpp"

namespace otbr {
namespace Android {

class AndroidRcpHost : public AndroidThreadHost
{
public:
    AndroidRcpHost(Host::RcpHost &aRcpHost);
    ~AndroidRcpHost(void) = default;

    void                         SetConfiguration(const OtDaemonConfiguration              &aConfiguration,
                                                  const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    const OtDaemonConfiguration &GetConfiguration(void) override { return mConfiguration; }
    void                         SetInfraLinkInterfaceName(const std::string                        &aInterfaceName,
                                                           int                                       aIcmp6Socket,
                                                           const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void                         SetInfraLinkNat64Prefix(const std::string                        &aNat64Prefix,
                                                         const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void                         SetInfraLinkDnsServers(const std::vector<std::string>           &aDnsServers,
                                                        const std::shared_ptr<IOtStatusReceiver> &aReceiver) override;
    void                         SetTrelEnabled(bool aEnabled) override;
    void                         RunOtCtlCommand(const std::string                        &aCommand,
                                                 const bool                                aIsInteractive,
                                                 const std::shared_ptr<IOtOutputReceiver> &aReceiver) override;
    binder_status_t              Dump(int aFd, const char **aArgs, uint32_t aNumArgs) override;

    void                   NotifyNat64PrefixDiscoveryDone(void);
    static AndroidRcpHost *Get(void) { return sAndroidRcpHost; }

private:
    otInstance *GetOtInstance(void);

    static otLinkModeConfig GetLinkModeConfig(bool aBeRouter);
    void                    SetBorderRouterEnabled(bool aEnabled);
    static int              OtCtlCommandCallback(void *aBinderServer, const char *aFormat, va_list aArguments);
    int                     OtCtlCommandCallback(const char *aFormat, va_list aArguments);

    static AndroidRcpHost *sAndroidRcpHost;

    Host::RcpHost        &mRcpHost;
    OtDaemonConfiguration mConfiguration;
    InfraLinkState        mInfraLinkState;
    int                   mInfraIcmp6Socket;
    bool                  mTrelEnabled;

    bool                               mIsOtCtlInteractiveMode;
    bool                               mIsOtCtlOutputComplete;
    std::shared_ptr<IOtOutputReceiver> mOtCtlOutputReceiver;
};

} // namespace Android
} // namespace otbr

#endif // OTBR_ANDROID_RCP_HOST_HPP_
