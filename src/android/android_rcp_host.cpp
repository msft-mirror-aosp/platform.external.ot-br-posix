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

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <openthread/backbone_router_ftd.h>
#include <openthread/border_routing.h>
#include <openthread/dnssd_server.h>
#include <openthread/ip6.h>
#include <openthread/nat64.h>
#include <openthread/openthread-system.h>
#include <openthread/srp_server.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>
#include <openthread/trel.h>
#include <openthread/platform/infra_if.h>
#include <openthread/platform/trel.h>

#include "android/common_utils.hpp"
#include "common/code_utils.hpp"

namespace otbr {
namespace Android {

AndroidRcpHost *AndroidRcpHost::sAndroidRcpHost = nullptr;

AndroidRcpHost::AndroidRcpHost(Host::RcpHost &aRcpHost)
    : mRcpHost(aRcpHost)
    , mConfiguration()
    , mInfraIcmp6Socket(-1)
{
    mInfraLinkState.interfaceName = "";

    sAndroidRcpHost = this;
}

void AndroidRcpHost::SetConfiguration(const OtDaemonConfiguration              &aConfiguration,
                                      const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    otError          error = OT_ERROR_NONE;
    std::string      message;
    otLinkModeConfig linkModeConfig;
    bool             borderRouterEnabled = aConfiguration.borderRouterEnabled;

    otbrLogInfo("Set configuration: %s", aConfiguration.toString().c_str());

    VerifyOrExit(GetOtInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    // TODO: b/343814054 - Support enabling/disabling DHCPv6-PD.
    VerifyOrExit(!aConfiguration.dhcpv6PdEnabled, error = OT_ERROR_NOT_IMPLEMENTED,
                 message = "DHCPv6-PD is not supported");
    otNat64SetEnabled(GetOtInstance(), aConfiguration.nat64Enabled);
    // DNS upstream query is enabled if and only if NAT64 is enabled.
    otDnssdUpstreamQuerySetEnabled(GetOtInstance(), aConfiguration.nat64Enabled);

    // Thread has to be a Router before new Android API is added to support making it a SED (Sleepy End Device)
    linkModeConfig = GetLinkModeConfig(/* aIsRouter= */ true);
    SuccessOrExit(error = otThreadSetLinkMode(GetOtInstance(), linkModeConfig), message = "Failed to set link mode");

    // - In non-BR mode, this device should try to be a router only when there are no other routers
    // - 16 is the default ROUTER_UPGRADE_THRESHOLD value defined in OpenThread
    otThreadSetRouterUpgradeThreshold(GetOtInstance(), (borderRouterEnabled ? 16 : 1));

    // Sets much lower Leader / Partition weight for a non-BR device so that it would
    // not attempt to be the new leader after merging partitions. Keeps BR using the
    // default Leader weight value 64.
    //
    // TODO: b/404979710 - sets leader weight higher based on the new Thread 1.4 device
    // properties feature.
    otThreadSetLocalLeaderWeight(GetOtInstance(), (borderRouterEnabled ? 64 : 32));

    if (borderRouterEnabled && aConfiguration.srpServerWaitForBorderRoutingEnabled)
    {
        // This will automatically disable fast-start mode if it was ever enabled
        otSrpServerSetAutoEnableMode(GetOtInstance(), true);
    }
    else
    {
        otSrpServerSetAutoEnableMode(GetOtInstance(), false);
        otSrpServerEnableFastStartMode(GetOtInstance());
    }

    SetBorderRouterEnabled(borderRouterEnabled);

    mConfiguration = aConfiguration;

exit:
    PropagateResult(error, message, aReceiver);
}

void AndroidRcpHost::SetInfraLinkInterfaceName(const std::string                        &aInterfaceName,
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

    SetTrelEnabled(mTrelEnabled);

exit:
    if (error != OT_ERROR_NONE)
    {
        close(aIcmp6Socket);
    }
    PropagateResult(error, message, aReceiver);
}

void AndroidRcpHost::SetTrelEnabled(bool aEnabled)
{
    mTrelEnabled = aEnabled;

    otbrLogInfo("%s TREL", aEnabled ? "Enabling" : "Disabling");

    // Tear down TREL if it's been initialized/enabled already.
    otTrelSetEnabled(GetOtInstance(), false);
    otSysTrelDeinit();

    if (mTrelEnabled && mInfraLinkState.interfaceName != "")
    {
        otSysTrelInit(mInfraLinkState.interfaceName.value_or("").c_str());
        otTrelSetEnabled(GetOtInstance(), true);
    }
}

void AndroidRcpHost::SetInfraLinkNat64Prefix(const std::string                        &aNat64Prefix,
                                             const std::shared_ptr<IOtStatusReceiver> &aReceiver)
{
    otError     error = OT_ERROR_NONE;
    std::string message;

    otbrLogInfo("Setting infra link NAT64 prefix: %s", aNat64Prefix.c_str());

    VerifyOrExit(mRcpHost.GetInstance() != nullptr, error = OT_ERROR_INVALID_STATE, message = "OT is not initialized");

    mInfraLinkState.nat64Prefix = aNat64Prefix;
    NotifyNat64PrefixDiscoveryDone();

exit:
    PropagateResult(error, message, aReceiver);
}

void AndroidRcpHost::RunOtCtlCommand(const std::string                        &aCommand,
                                     const bool                                aIsInteractive,
                                     const std::shared_ptr<IOtOutputReceiver> &aReceiver)
{
    otSysCliInitUsingDaemon(GetOtInstance());

    if (!aCommand.empty())
    {
        std::string command = aCommand;

        mIsOtCtlInteractiveMode = aIsInteractive;
        mOtCtlOutputReceiver    = aReceiver;

        otCliInit(GetOtInstance(), AndroidRcpHost::OtCtlCommandCallback, this);
        otCliInputLine(command.data());
    }
}

int AndroidRcpHost::OtCtlCommandCallback(void *aBinderServer, const char *aFormat, va_list aArguments)
{
    return static_cast<AndroidRcpHost *>(aBinderServer)->OtCtlCommandCallback(aFormat, aArguments);
}

int AndroidRcpHost::OtCtlCommandCallback(const char *aFormat, va_list aArguments)
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

binder_status_t AndroidRcpHost::Dump(int aFd, const char **aArgs, uint32_t aNumArgs)
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

void AndroidRcpHost::SetInfraLinkDnsServers(const std::vector<std::string>           &aDnsServers,
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

void AndroidRcpHost::NotifyNat64PrefixDiscoveryDone(void)
{
    otIp6Prefix nat64Prefix{};
    uint32_t    infraIfIndex = if_nametoindex(mInfraLinkState.interfaceName.value_or("").c_str());

    otIp6PrefixFromString(mInfraLinkState.nat64Prefix.value_or("").c_str(), &nat64Prefix);
    otPlatInfraIfDiscoverNat64PrefixDone(GetOtInstance(), infraIfIndex, &nat64Prefix);
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

void AndroidRcpHost::SetBorderRouterEnabled(bool aEnabled)
{
    otError error;

    error = otBorderRoutingSetEnabled(GetOtInstance(), aEnabled);
    if (error != OT_ERROR_NONE)
    {
        otbrLogWarning("Failed to %s Border Routing: %s", (aEnabled ? "enable" : "disable"),
                       otThreadErrorToString(error));
        ExitNow();
    }

    otBackboneRouterSetEnabled(GetOtInstance(), aEnabled);

exit:
    return;
}

extern "C" otError otPlatInfraIfDiscoverNat64Prefix(uint32_t aInfraIfIndex)
{
    OT_UNUSED_VARIABLE(aInfraIfIndex);

    AndroidRcpHost *androidRcpHost = AndroidRcpHost::Get();
    otError         error          = OT_ERROR_NONE;

    VerifyOrExit(androidRcpHost != nullptr, error = OT_ERROR_INVALID_STATE);

    androidRcpHost->NotifyNat64PrefixDiscoveryDone();

exit:
    return error;
}

} // namespace Android
} // namespace otbr
