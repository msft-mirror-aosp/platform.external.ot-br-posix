/*
 *    Copyright (c) 2024, The OpenThread Authors.
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

#ifndef OTBR_ANDROID_THREAD_HOST_HPP_
#define OTBR_ANDROID_THREAD_HOST_HPP_

#include <memory>

#include "common_utils.hpp"

namespace otbr {
namespace Android {

class AndroidThreadHost
{
public:
    virtual ~AndroidThreadHost(void) = default;

    virtual void                         SetConfiguration(const OtDaemonConfiguration              &aConfiguration,
                                                          const std::shared_ptr<IOtStatusReceiver> &aReceiver) = 0;
    virtual const OtDaemonConfiguration &GetConfiguration(void)                                                = 0;
    virtual void                         SetInfraLinkInterfaceName(const std::string                        &aInterfaceName,
                                                                   int                                       aIcmp6Socket,
                                                                   const std::shared_ptr<IOtStatusReceiver> &aReceiver) = 0;
    virtual void                         SetInfraLinkNat64Prefix(const std::string                        &aNat64Prefix,
                                                                 const std::shared_ptr<IOtStatusReceiver> &aReceiver) = 0;
    virtual void                         SetInfraLinkDnsServers(const std::vector<std::string>           &aDnsServers,
                                                                const std::shared_ptr<IOtStatusReceiver> &aReceiver) = 0;
    virtual void                         SetTrelEnabled(bool aEnabled) = 0;
};

} // namespace Android
} // namespace otbr

#endif // OTBR_ANDROID_THREAD_HOST_HPP_
