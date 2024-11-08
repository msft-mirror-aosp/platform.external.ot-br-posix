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

#define OTBR_LOG_TAG "ARCP_HOST"

#include "android_rcp_host.hpp"

#include <net/if.h>
#include <vector>

#include <openthread/backbone_router_ftd.h>
#include <openthread/border_routing.h>
#include <openthread/dnssd_server.h>
#include <openthread/ip6.h>
#include <openthread/nat64.h>
#include <openthread/openthread-system.h>
#include <openthread/srp_server.h>
#include <openthread/thread.h>
#include <openthread/platform/infra_if.h>

#include "android/common_utils.hpp"
#include "common/code_utils.hpp"

namespace otbr {
namespace Android {

AndroidRcpHost::AndroidRcpHost(Ncp::RcpHost &aRcpHost)
    : mRcpHost(aRcpHost)
    , mConfiguration()
{
}

void AndroidRcpHost::SetConfiguration(const OtDaemonConfiguration              &aConfiguration,
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

otInstance *AndroidRcpHost::GetOtInstance(void)
{
    return mRcpHost.GetInstance();
}

otLinkModeConfig AndroidRcpHost::GetLinkModeConfig(bool aIsRouter)
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

} // namespace Android
} // namespace otbr
