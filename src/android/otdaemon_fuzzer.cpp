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

#include <fuzzbinder/libbinder_ndk_driver.h>
#include <fuzzer/FuzzedDataProvider.h>

#include "android/mdns_publisher.hpp"
#include "android/otdaemon_server.hpp"
#include "host/rcp_host.hpp"
#include "mdns/mdns.hpp"
#include "sdp_proxy/advertising_proxy.hpp"

using android::fuzzService;
using otbr::Android::MdnsPublisher;
using otbr::Android::OtDaemonServer;
using otbr::Host::RcpHost;
using otbr::Mdns::Publisher;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    RcpHost                rcpHost       = RcpHost{"" /* aInterfaceName */,
                              {"threadnetwork_hal://binder?none"},
                              "" /* aBackboneInterfaceName */,
                              true /* aDryRun */,
                              false /* aEnableAutoAttach*/};
    auto                   mdnsPublisher = static_cast<MdnsPublisher *>(Publisher::Create([](Publisher::State) {}));
    otbr::BorderAgent      borderAgent{rcpHost, *mdnsPublisher};
    otbr::AdvertisingProxy advProxy{rcpHost, *mdnsPublisher};

    auto service = ndk::SharedRefBase::make<OtDaemonServer>(rcpHost, *mdnsPublisher, borderAgent, advProxy, []() {});
    fuzzService(service->asBinder().get(), FuzzedDataProvider(data, size));
    return 0;
}

extern "C" void otPlatReset(otInstance *aInstance)
{
}
