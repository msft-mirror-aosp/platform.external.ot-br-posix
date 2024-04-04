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

#define OTBR_LOG_TAG "MDNS"

#include "android/mdns_publisher.hpp"

namespace otbr {

Mdns::Publisher *Mdns::Publisher::Create(Mdns::Publisher::StateCallback aCallback)
{
    return new Android::MdnsPublisher(std::move(aCallback));
}

namespace Android {
otbrError DnsErrorToOtbrErrorImpl(int32_t aError)
{
    return aError == 0 ? OTBR_ERROR_NONE : OTBR_ERROR_MDNS;
}

otbrError MdnsPublisher::DnsErrorToOtbrError(int32_t aError)
{
    return DnsErrorToOtbrErrorImpl(aError);
}

Status MdnsPublisher::NsdStatusReceiver::onSuccess()
{
    if (!mCallback.IsNull())
    {
        std::move(mCallback)(OTBR_ERROR_NONE);
    }

    return Status::ok();
}

Status MdnsPublisher::NsdDiscoverServiceCallback::onServiceDiscovered(const std::string &aName,
                                                                      const std::string &aType,
                                                                      bool               aIsFound)
{
    VerifyOrExit(aIsFound, mSubscription.mPublisher.OnServiceRemoved(0, aType, aName));

    mSubscription.Resolve(aName, aType);

exit:
    return Status::ok();
}

Status MdnsPublisher::NsdResolveServiceCallback::onServiceResolved(const std::string                  &aHostname,
                                                                   const std::string                  &aName,
                                                                   const std::string                  &aType,
                                                                   int                                 aPort,
                                                                   const std::vector<std::string>     &aAddresses,
                                                                   const std::vector<DnsTxtAttribute> &aTxt,
                                                                   int                                 aTtlSeconds)
{
    DiscoveredInstanceInfo info;
    TxtList                txtList;

    info.mHostName = aHostname + ".local.";
    info.mName     = aName;
    info.mPort     = aPort;
    info.mTtl      = std::clamp(aTtlSeconds, kMinResolvedTtl, kMaxResolvedTtl);
    for (const auto &addressStr : aAddresses)
    {
        Ip6Address address;
        int        error = Ip6Address::FromString(addressStr.c_str(), address);

        if (error != OTBR_ERROR_NONE)
        {
            otbrLogInfo("Failed to parse resolved IPv6 address: %s", addressStr.c_str());
            continue;
        }
        info.mAddresses.push_back(address);
    }
    for (const auto &entry : aTxt)
    {
        txtList.emplace_back(entry.name.c_str(), entry.value.data(), entry.value.size());
    }
    EncodeTxtData(txtList, info.mTxtData);

    mSubscription.mPublisher.OnServiceResolved(aType, info);

    return Status::ok();
}

void MdnsPublisher::SetINsdPublisher(std::shared_ptr<INsdPublisher> aINsdPublisher)
{
    otbrLogInfo("Set INsdPublisher %p", aINsdPublisher.get());

    mNsdPublisher = std::move(aINsdPublisher);

    if (mNsdPublisher != nullptr)
    {
        mStateCallback(Mdns::Publisher::State::kReady);
    }
    else
    {
        mStateCallback(Mdns::Publisher::State::kIdle);
    }
}

Status MdnsPublisher::NsdStatusReceiver::onError(int aError)
{
    if (!mCallback.IsNull())
    {
        std::move(mCallback)(DnsErrorToOtbrErrorImpl(aError));
    }

    return Status::ok();
}

std::shared_ptr<MdnsPublisher::NsdStatusReceiver> CreateReceiver(Mdns::Publisher::ResultCallback aCallback)
{
    return ndk::SharedRefBase::make<MdnsPublisher::NsdStatusReceiver>(std::move(aCallback));
}

std::shared_ptr<MdnsPublisher::NsdDiscoverServiceCallback> CreateNsdDiscoverServiceCallback(
    MdnsPublisher::ServiceSubscription &aServiceSubscription)
{
    return ndk::SharedRefBase::make<MdnsPublisher::NsdDiscoverServiceCallback>(aServiceSubscription);
}

std::shared_ptr<MdnsPublisher::NsdResolveServiceCallback> CreateNsdResolveServiceCallback(
    MdnsPublisher::ServiceSubscription &aServiceSubscription)
{
    return ndk::SharedRefBase::make<MdnsPublisher::NsdResolveServiceCallback>(aServiceSubscription);
}

void DieForNotImplemented(const char *aFuncName)
{
    VerifyOrDie(false, (std::string(aFuncName) + " is not implemented").c_str());
}

otbrError MdnsPublisher::PublishServiceImpl(const std::string &aHostName,
                                            const std::string &aName,
                                            const std::string &aType,
                                            const SubTypeList &aSubTypeList,
                                            uint16_t           aPort,
                                            const TxtData     &aTxtData,
                                            ResultCallback   &&aCallback)
{
    int32_t   listenerId = AllocateListenerId();
    TxtList   txtList;
    otbrError error = OTBR_ERROR_NONE;

    std::vector<DnsTxtAttribute> txtAttributes;

    VerifyOrExit(IsStarted(), error = OTBR_ERROR_MDNS);
    if (mNsdPublisher == nullptr)
    {
        otbrLogWarning("No platform mDNS implementation registered!");
        ExitNow(error = OTBR_ERROR_MDNS);
    }

    aCallback = HandleDuplicateServiceRegistration(aHostName, aName, aType, aSubTypeList, aPort, aTxtData,
                                                   std::move(aCallback));
    VerifyOrExit(!aCallback.IsNull(), error = OTBR_ERROR_INVALID_STATE);

    SuccessOrExit(error = DecodeTxtData(txtList, aTxtData.data(), aTxtData.size()));

    for (const auto &txtEntry : txtList)
    {
        DnsTxtAttribute txtAttribute;

        txtAttribute.name  = txtEntry.mKey;
        txtAttribute.value = txtEntry.mValue;
        txtAttributes.push_back(std::move(txtAttribute));
    }
    AddServiceRegistration(MakeUnique<NsdServiceRegistration>(aHostName, aName, aType, aSubTypeList, aPort, aTxtData,
                                                              /* aCallback= */ nullptr, this, listenerId,
                                                              mNsdPublisher));

    otbrLogInfo("Publishing service %s.%s listener ID = %d", aName.c_str(), aType.c_str(), listenerId);

    mNsdPublisher->registerService(aHostName, aName, aType, aSubTypeList, aPort, txtAttributes,
                                   CreateReceiver(std::move(aCallback)), listenerId);

exit:
    return error;
}

void MdnsPublisher::UnpublishService(const std::string &aName, const std::string &aType, ResultCallback &&aCallback)
{
    NsdServiceRegistration *serviceRegistration =
        static_cast<NsdServiceRegistration *>(FindServiceRegistration(aName, aType));

    VerifyOrExit(IsStarted(), std::move(aCallback)(OTBR_ERROR_MDNS));
    if (mNsdPublisher == nullptr)
    {
        otbrLogWarning("No platform mDNS implementation registered!");
        ExitNow(std::move(aCallback)(OTBR_ERROR_MDNS));
    }
    VerifyOrExit(serviceRegistration != nullptr, std::move(aCallback)(OTBR_ERROR_NONE));

    serviceRegistration->mUnregisterReceiver = CreateReceiver(std::move(aCallback));
    RemoveServiceRegistration(aName, aType, OTBR_ERROR_NONE);

exit:
    return;
}

otbrError MdnsPublisher::PublishHostImpl(const std::string &aName,
                                         const AddressList &aAddresses,
                                         ResultCallback   &&aCallback)
{
    int32_t   listenerId = AllocateListenerId();
    TxtList   txtList;
    otbrError error = OTBR_ERROR_NONE;

    std::vector<std::string> addressStrings;

    VerifyOrExit(IsStarted(), error = OTBR_ERROR_MDNS);
    if (mNsdPublisher == nullptr)
    {
        otbrLogWarning("No platform mDNS implementation registered!");
        ExitNow(error = OTBR_ERROR_MDNS);
    }

    aCallback = HandleDuplicateHostRegistration(aName, aAddresses, std::move(aCallback));
    VerifyOrExit(!aCallback.IsNull(), error = OTBR_ERROR_INVALID_STATE);

    AddHostRegistration(
        MakeUnique<NsdHostRegistration>(aName, aAddresses, /* aCallback= */ nullptr, this, listenerId, mNsdPublisher));

    otbrLogInfo("Publishing host %s listener ID = %d", aName.c_str(), listenerId);

    addressStrings.reserve(aAddresses.size());
    for (const Ip6Address &address : aAddresses)
    {
        addressStrings.push_back(address.ToString());
    }

    if (aAddresses.size())
    {
        mNsdPublisher->registerHost(aName, addressStrings, CreateReceiver(std::move(aCallback)), listenerId);
    }
    else
    {
        // No addresses to register.
        std::move(aCallback)(OTBR_ERROR_NONE);
    }

exit:
    return error;
}

otbrError MdnsPublisher::PublishKeyImpl(const std::string &aName, const KeyData &aKeyData, ResultCallback &&aCallback)
{
    OTBR_UNUSED_VARIABLE(aName);
    OTBR_UNUSED_VARIABLE(aKeyData);
    OTBR_UNUSED_VARIABLE(aCallback);

    DieForNotImplemented(__func__);

    return OTBR_ERROR_MDNS;
}

void MdnsPublisher::UnpublishHost(const std::string &aName, ResultCallback &&aCallback)
{
    NsdHostRegistration *hostRegistration = static_cast<NsdHostRegistration *>(FindHostRegistration(aName));

    VerifyOrExit(IsStarted(), std::move(aCallback)(OTBR_ERROR_MDNS));
    if (mNsdPublisher == nullptr)
    {
        otbrLogWarning("No platform mDNS implementation registered!");
        ExitNow(std::move(aCallback)(OTBR_ERROR_MDNS));
    }
    VerifyOrExit(hostRegistration != nullptr, std::move(aCallback)(OTBR_ERROR_NONE));

    hostRegistration->mUnregisterReceiver = CreateReceiver(std::move(aCallback));
    RemoveHostRegistration(aName, OTBR_ERROR_NONE);

exit:
    return;
}

void MdnsPublisher::UnpublishKey(const std::string &aName, ResultCallback &&aCallback)
{
    OTBR_UNUSED_VARIABLE(aName);
    OTBR_UNUSED_VARIABLE(aCallback);

    DieForNotImplemented(__func__);
}

void MdnsPublisher::SubscribeService(const std::string &aType, const std::string &aInstanceName)
{
    auto service = MakeUnique<ServiceSubscription>(aType, aInstanceName, *this, mNsdPublisher);

    VerifyOrExit(IsStarted(), otbrLogWarning("No platform mDNS implementation registered!"));

    mServiceSubscriptions.push_back(std::move(service));

    otbrLogInfo("Subscribe service %s.%s (total %zu)", aInstanceName.c_str(), aType.c_str(),
                mServiceSubscriptions.size());

    if (aInstanceName.empty())
    {
        mServiceSubscriptions.back()->Browse();
    }
    else
    {
        mServiceSubscriptions.back()->Resolve(aInstanceName, aType);
    }
exit:
    return;
}

void MdnsPublisher::UnsubscribeService(const std::string &aType, const std::string &aInstanceName)
{
    ServiceSubscriptionList::iterator it;

    VerifyOrExit(IsStarted());

    it = std::find_if(mServiceSubscriptions.begin(), mServiceSubscriptions.end(),
                      [&aType, &aInstanceName](const std::unique_ptr<ServiceSubscription> &aService) {
                          return aService->mType == aType && aService->mName == aInstanceName;
                      });

    VerifyOrExit(it != mServiceSubscriptions.end(),
                 otbrLogWarning("The service %s.%s is already unsubscribed.", aInstanceName.c_str(), aType.c_str()));

    {
        std::unique_ptr<ServiceSubscription> service = std::move(*it);

        mServiceSubscriptions.erase(it);
    }

    otbrLogInfo("Unsubscribe service %s.%s (left %zu)", aInstanceName.c_str(), aType.c_str(),
                mServiceSubscriptions.size());

exit:
    return;
}

void MdnsPublisher::SubscribeHost(const std::string &aHostName)
{
    OTBR_UNUSED_VARIABLE(aHostName);
}

void MdnsPublisher::UnsubscribeHost(const std::string &aHostName)
{
    OTBR_UNUSED_VARIABLE(aHostName);
}

void MdnsPublisher::OnServiceResolveFailedImpl(const std::string &aType,
                                               const std::string &aInstanceName,
                                               int32_t            aErrorCode)
{
    OTBR_UNUSED_VARIABLE(aType);
    OTBR_UNUSED_VARIABLE(aInstanceName);
    OTBR_UNUSED_VARIABLE(aErrorCode);

    DieForNotImplemented(__func__);
}

void MdnsPublisher::OnHostResolveFailedImpl(const std::string &aHostName, int32_t aErrorCode)
{
    OTBR_UNUSED_VARIABLE(aHostName);
    OTBR_UNUSED_VARIABLE(aErrorCode);

    DieForNotImplemented(__func__);
}

int32_t MdnsPublisher::AllocateListenerId(void)
{
    if (mNextListenerId == std::numeric_limits<int32_t>::max())
    {
        mNextListenerId = 0;
    }
    return mNextListenerId++;
}

MdnsPublisher::NsdServiceRegistration::~NsdServiceRegistration(void)
{
    auto nsdPublisher = mNsdPublisher.lock();

    VerifyOrExit(mPublisher->IsStarted() && nsdPublisher != nullptr);

    otbrLogInfo("Unpublishing service %s.%s listener ID = %d", mName.c_str(), mType.c_str(), mListenerId);

    if (!mUnregisterReceiver)
    {
        mUnregisterReceiver = CreateReceiver([](int) {});
    }

    nsdPublisher->unregister(mUnregisterReceiver, mListenerId);

exit:
    return;
}

MdnsPublisher::NsdHostRegistration::~NsdHostRegistration(void)
{
    auto nsdPublisher = mNsdPublisher.lock();

    VerifyOrExit(mPublisher->IsStarted() && nsdPublisher != nullptr);

    otbrLogInfo("Unpublishing host %s listener ID = %d", mName.c_str(), mListenerId);

    if (!mUnregisterReceiver)
    {
        mUnregisterReceiver = CreateReceiver([](int) {});
    }

    nsdPublisher->unregister(mUnregisterReceiver, mListenerId);

exit:
    return;
}

void MdnsPublisher::ServiceSubscription::Release(void)
{
    otbrLogInfo("Browsing service type %s", mType.c_str());

    std::vector<std::string> instanceNames;

    for (const auto &nameAndResolvers : mResolvers)
    {
        instanceNames.push_back(nameAndResolvers.first);
    }
    for (const auto &name : instanceNames)
    {
        RemoveServiceResolver(name);
    }

    mNsdPublisher->stopServiceDiscovery(mBrowseListenerId);
}

void MdnsPublisher::ServiceSubscription::Browse(void)
{
    VerifyOrExit(mPublisher.IsStarted());

    otbrLogInfo("Browsing service type %s", mType.c_str());

    mNsdPublisher->discoverService(mType, CreateNsdDiscoverServiceCallback(*this), mBrowseListenerId);

exit:
    return;
}

void MdnsPublisher::ServiceSubscription::Resolve(const std::string &aName, const std::string &aType)
{
    ServiceResolver *resolver = new ServiceResolver(mPublisher.AllocateListenerId(), mNsdPublisher);

    VerifyOrExit(mPublisher.IsStarted());

    otbrLogInfo("Resolving service %s.%s", aName.c_str(), aType.c_str());

    AddServiceResolver(aName, resolver);
    mNsdPublisher->resolveService(aName, aType, CreateNsdResolveServiceCallback(*this), resolver->mListenerId);

exit:
    return;
}

void MdnsPublisher::ServiceSubscription::AddServiceResolver(const std::string &aName, ServiceResolver *aResolver)
{
    mResolvers[aName].insert(aResolver);
}
void MdnsPublisher::ServiceSubscription::RemoveServiceResolver(const std::string &aName)
{
    int numResolvers = 0;

    VerifyOrExit(mResolvers.find(aName) != mResolvers.end());

    numResolvers = mResolvers[aName].size();

    for (auto resolver : mResolvers[aName])
    {
        delete resolver;
    }

    mResolvers.erase(aName);

exit:
    otbrLogDebug("Removed %d service resolver for instance %s", numResolvers, aName.c_str());
    return;
}

} // namespace Android
} // namespace otbr
