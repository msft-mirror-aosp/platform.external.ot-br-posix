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

#ifndef OTBR_ANDROID_MDNS_PUBLISHER_HPP_
#define OTBR_ANDROID_MDNS_PUBLISHER_HPP_

#include "mdns/mdns.hpp"

#include <aidl/com/android/server/thread/openthread/BnNsdDiscoverServiceCallback.h>
#include <aidl/com/android/server/thread/openthread/BnNsdResolveHostCallback.h>
#include <aidl/com/android/server/thread/openthread/BnNsdResolveServiceCallback.h>
#include <aidl/com/android/server/thread/openthread/BnNsdStatusReceiver.h>
#include <aidl/com/android/server/thread/openthread/DnsTxtAttribute.h>
#include <aidl/com/android/server/thread/openthread/INsdPublisher.h>
#include <set>

namespace otbr {
namespace Android {
using Status = ::ndk::ScopedAStatus;
using aidl::com::android::server::thread::openthread::BnNsdDiscoverServiceCallback;
using aidl::com::android::server::thread::openthread::BnNsdResolveHostCallback;
using aidl::com::android::server::thread::openthread::BnNsdResolveServiceCallback;
using aidl::com::android::server::thread::openthread::BnNsdStatusReceiver;
using aidl::com::android::server::thread::openthread::DnsTxtAttribute;
using aidl::com::android::server::thread::openthread::INsdPublisher;

class MdnsPublisher : public Mdns::Publisher
{
public:
    explicit MdnsPublisher(Publisher::StateCallback aCallback)
        : mStateCallback(std::move(aCallback))
        , mNextListenerId(0)

    {
    }

    ~MdnsPublisher(void) { Stop(); }

    /** Sets the INsdPublisher which forwards the mDNS API requests to the NsdManager in system_server. */
    void SetINsdPublisher(std::shared_ptr<INsdPublisher> aINsdPublisher);

    otbrError Start(void) override { return OTBR_ERROR_NONE; }

    void Stop(void) override
    {
        mServiceRegistrations.clear();
        mHostRegistrations.clear();
        if (mNsdPublisher != nullptr)
        {
            mNsdPublisher->reset();
        }
    }

    bool IsStarted(void) const override { return mNsdPublisher != nullptr; }

    void UnpublishService(const std::string &aName, const std::string &aType, ResultCallback &&aCallback) override;

    void UnpublishHost(const std::string &aName, ResultCallback &&aCallback) override;

    void UnpublishKey(const std::string &aName, ResultCallback &&aCallback) override;

    void SubscribeService(const std::string &aType, const std::string &aInstanceName) override;

    void UnsubscribeService(const std::string &aType, const std::string &aInstanceName) override;

    void SubscribeHost(const std::string &aHostName) override;

    void UnsubscribeHost(const std::string &aHostName) override;

    class NsdStatusReceiver : public BnNsdStatusReceiver
    {
    public:
        explicit NsdStatusReceiver(Mdns::Publisher::ResultCallback aCallback)
            : mCallback(std::move(aCallback))
        {
        }

        Status onSuccess(void) override;

        Status onError(int aError) override;

    private:
        Mdns::Publisher::ResultCallback mCallback;
    };

    struct ServiceResolver : private ::NonCopyable
    {
        explicit ServiceResolver(int aListenerId, std::shared_ptr<INsdPublisher> aNsdPublisher)
            : mListenerId(aListenerId)
            , mNsdPublisher(std::move(aNsdPublisher))
        {
        }

        ~ServiceResolver(void)
        {
            if (mNsdPublisher)
            {
                mNsdPublisher->stopServiceResolution(mListenerId);
            }
        }

        int                            mListenerId;
        std::shared_ptr<INsdPublisher> mNsdPublisher;
    };

    struct ServiceSubscription : private ::NonCopyable
    {
        explicit ServiceSubscription(std::string                    aType,
                                     std::string                    aName,
                                     MdnsPublisher                 &aPublisher,
                                     std::shared_ptr<INsdPublisher> aNsdPublisher)
            : mType(std::move(aType))
            , mName(std::move(aName))
            , mPublisher(aPublisher)
            , mNsdPublisher(std::move(aNsdPublisher))
            , mBrowseListenerId(-1)
        {
        }

        ~ServiceSubscription(void) { Release(); }

        void Release(void);
        void Browse(void);
        void Resolve(const std::string &aName, const std::string &aType);
        void AddServiceResolver(const std::string &aName, ServiceResolver *aResolver);
        void RemoveServiceResolver(const std::string &aInstanceName);

        std::string                    mType;
        std::string                    mName;
        MdnsPublisher                 &mPublisher;
        std::shared_ptr<INsdPublisher> mNsdPublisher;
        int32_t                        mBrowseListenerId;

        std::map<std::string, std::set<ServiceResolver *>> mResolvers;
    };

    struct HostSubscription : private ::NonCopyable
    {
        explicit HostSubscription(std::string                    aName,
                                  MdnsPublisher                 &aPublisher,
                                  std::shared_ptr<INsdPublisher> aNsdPublisher,
                                  int                            listenerId)
            : mName(std::move(aName))
            , mPublisher(aPublisher)
            , mNsdPublisher(std::move(aNsdPublisher))
            , mListenerId(listenerId)
        {
        }

        ~HostSubscription(void) { Release(); }

        void Release(void) { mNsdPublisher->stopHostResolution(mListenerId); }

        std::string                    mName;
        MdnsPublisher                 &mPublisher;
        std::shared_ptr<INsdPublisher> mNsdPublisher;
        const int32_t                  mListenerId;
    };

    class NsdDiscoverServiceCallback : public BnNsdDiscoverServiceCallback
    {
    public:
        explicit NsdDiscoverServiceCallback(ServiceSubscription &aSubscription)
            : mSubscription(aSubscription)
        {
        }

        Status onServiceDiscovered(const std::string &aName, const std::string &aType, bool aIsFound);

    private:
        ServiceSubscription &mSubscription;
    };

    class NsdResolveServiceCallback : public BnNsdResolveServiceCallback
    {
    public:
        explicit NsdResolveServiceCallback(ServiceSubscription &aSubscription)
            : mSubscription(aSubscription)
        {
        }

        Status onServiceResolved(const std::string                  &aHostname,
                                 int                                 aNetifIndex,
                                 const std::string                  &aName,
                                 const std::string                  &aType,
                                 int                                 aPort,
                                 const std::vector<std::string>     &aAddresses,
                                 const std::vector<DnsTxtAttribute> &aTxt,
                                 int                                 aTtlSeconds);

    private:
        ServiceSubscription &mSubscription;
    };

    class NsdResolveHostCallback : public BnNsdResolveHostCallback
    {
    public:
        explicit NsdResolveHostCallback(HostSubscription &aSubscription)
            : mSubscription(aSubscription)
        {
        }

        Status onHostResolved(const std::string &aName, const std::vector<std::string> &aAddresses);

    private:
        HostSubscription &mSubscription;
    };

protected:
    otbrError PublishServiceImpl(const std::string &aHostName,
                                 const std::string &aName,
                                 const std::string &aType,
                                 const SubTypeList &aSubTypeList,
                                 uint16_t           aPort,
                                 const TxtData     &aTxtData,
                                 ResultCallback   &&aCallback) override;

    otbrError PublishHostImpl(const std::string &aName, const AddressList &aAddresses, ResultCallback &&aCallback);

    otbrError PublishKeyImpl(const std::string &aName, const KeyData &aKeyData, ResultCallback &&aCallback) override;

    void OnServiceResolveFailedImpl(const std::string &aType, const std::string &aInstanceName, int32_t aErrorCode);

    void OnHostResolveFailedImpl(const std::string &aHostName, int32_t aErrorCode);

    otbrError DnsErrorToOtbrError(int32_t aError);

private:
    class NsdServiceRegistration : public ServiceRegistration
    {
    public:
        NsdServiceRegistration(const std::string           &aHostName,
                               const std::string           &aName,
                               const std::string           &aType,
                               const SubTypeList           &aSubTypeList,
                               uint16_t                     aPort,
                               const TxtData               &aTxtData,
                               ResultCallback             &&aCallback,
                               MdnsPublisher               *aPublisher,
                               int32_t                      aListenerId,
                               std::weak_ptr<INsdPublisher> aINsdPublisher)
            : ServiceRegistration(aHostName,
                                  aName,
                                  aType,
                                  aSubTypeList,
                                  aPort,
                                  aTxtData,
                                  std::move(aCallback),
                                  aPublisher)
            , mListenerId(aListenerId)
            , mNsdPublisher(std::move(aINsdPublisher))

        {
        }

        ~NsdServiceRegistration(void) override;

        const int32_t                      mListenerId;
        std::shared_ptr<NsdStatusReceiver> mUnregisterReceiver;

    private:
        std::weak_ptr<INsdPublisher> mNsdPublisher;
    };

    class NsdHostRegistration : public HostRegistration
    {
    public:
        NsdHostRegistration(const std::string           &aName,
                            const AddressList           &aAddresses,
                            ResultCallback             &&aCallback,
                            MdnsPublisher               *aPublisher,
                            int32_t                      aListenerId,
                            std::weak_ptr<INsdPublisher> aINsdPublisher)
            : HostRegistration(aName, aAddresses, std::move(aCallback), aPublisher)
            , mListenerId(aListenerId)
            , mNsdPublisher(aINsdPublisher)
        {
        }

        ~NsdHostRegistration(void) override;

        const int32_t                      mListenerId;
        std::shared_ptr<NsdStatusReceiver> mUnregisterReceiver;

    private:
        std::weak_ptr<INsdPublisher> mNsdPublisher;
    };

    typedef std::vector<std::unique_ptr<ServiceSubscription>> ServiceSubscriptionList;
    typedef std::vector<std::unique_ptr<HostSubscription>>    HostSubscriptionList;

    static constexpr int kDefaultResolvedTtl = 10;
    static constexpr int kMinResolvedTtl     = 1;
    static constexpr int kMaxResolvedTtl     = 10;

    int32_t AllocateListenerId(void);

    StateCallback                  mStateCallback;
    int32_t                        mNextListenerId;
    std::shared_ptr<INsdPublisher> mNsdPublisher = nullptr;
    ServiceSubscriptionList        mServiceSubscriptions;
    HostSubscriptionList           mHostSubscriptions;
};

} // namespace Android
} // namespace otbr

#endif // OTBR_ANDROID_MDNS_PUBLISHER_HPP_
