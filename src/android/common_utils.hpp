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

#ifndef COMMON_UTILS_HPP_
#define COMMON_UTILS_HPP_

#include <aidl/com/android/server/thread/openthread/BnOtDaemon.h>
#include <aidl/com/android/server/thread/openthread/INsdPublisher.h>
#include <aidl/com/android/server/thread/openthread/IOtDaemon.h>
#include <aidl/com/android/server/thread/openthread/InfraLinkState.h>

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

void PropagateResult(int aError, const std::string &aMessage, const std::shared_ptr<IOtStatusReceiver> &aReceiver);

} // namespace Android
} // namespace otbr

#endif // COMMON_UTILS_HPP_
