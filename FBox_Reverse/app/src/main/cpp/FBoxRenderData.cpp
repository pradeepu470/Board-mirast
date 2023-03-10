//DEVELOP BY PRADEEP 28/03/2021

#define LOG_NDEBUG 0
#define LOG_TAG "FBoxRenderData"
#include <utils/Log.h>

#include "FBoxRenderData.h"

#include "ATSParser.h"
#include "FboxUtils.h"
#include <binder/IMemory.h>
#include <binder/IServiceManager.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>
#include <media/IMediaPlayerService.h>
#include <media/IStreamSource.h>
#include <media/mediaplayer.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <ui/DisplayInfo.h>
#include <cutils/properties.h>
#include <media/ammediaplayerext.h>

namespace android
{
    struct FBoxRenderData::PlayerClient : public BnMediaPlayerClient
    {
        PlayerClient()
            :mPrepared(false)
        {

        }

        virtual void notify(int msg, int ext1, int ext2, const Parcel *obj)
        {
            ALOGI("notify %d, %d, %d, %p", msg, ext1, ext2, obj);
            switch (msg) {
            case MEDIA_PREPARED:
                mPrepared = true;
                break;
            default:
                break;
            }
        }
        bool isPrepared() {
            return mPrepared;
        }

    protected:
        virtual ~PlayerClient()
        {
            ALOGI("~PlayerClient");
        }

    private:
        bool mPrepared;
        DISALLOW_EVIL_CONSTRUCTORS(PlayerClient);
    };

    struct FBoxRenderData::StreamSource : public BnStreamSource
    {
        StreamSource(FBoxRenderData *owner);

        virtual void setListener(const sp<IStreamListener> &listener);
        virtual void setBuffers(const Vector<sp<IMemory> > &buffers);

        virtual void onBufferAvailable(size_t index);

        virtual uint32_t flags() const;

        void doSomeWork();

    protected:
        virtual ~StreamSource();

    private:
        mutable Mutex mLock;

        FBoxRenderData *mOwner;

        sp<IStreamListener> mListener;

        Vector<sp<IMemory> > mBuffers;
        List<size_t> mIndicesAvailable;

        size_t mNumDeqeued;

        DISALLOW_EVIL_CONSTRUCTORS(StreamSource);
    };

    FBoxRenderData::StreamSource::StreamSource(FBoxRenderData *owner)
        : mOwner(owner),
          mNumDeqeued(0)
    {
    }

    FBoxRenderData::StreamSource::~StreamSource()
    {
        ALOGI("~StreamSource");
    }

    void FBoxRenderData::StreamSource::setListener(
        const sp<IStreamListener> &listener)
    {
        mListener = listener;
    }

    void FBoxRenderData::StreamSource::setBuffers(
        const Vector<sp<IMemory> > &buffers)
    {
        mBuffers = buffers;
    }

    void FBoxRenderData::StreamSource::onBufferAvailable(size_t index)
    {
        if (index >= 0x80000000)
        {
            doSomeWork();
            return ;
        }
        CHECK_LT(index, mBuffers.size());

        {
            Mutex::Autolock autoLock(mLock);
            mIndicesAvailable.push_back(index);
        }

        doSomeWork();
    }

    uint32_t FBoxRenderData::StreamSource::flags() const
    {
        return kFlagAlignedVideoData;
    }

    void FBoxRenderData::StreamSource::doSomeWork()
    {
        Mutex::Autolock autoLock(mLock);

        while (!mIndicesAvailable.empty())
        {
            sp<ABuffer> srcBuffer = mOwner->dequeueBuffer();
            if (srcBuffer == NULL)
            {
                break;
            }

            ++mNumDeqeued;

            if (mNumDeqeued == 1)
            {
                ALOGI("fixing real time now.");

                sp<AMessage> extra = new AMessage;

                extra->setInt64("timeUs", ALooper::GetNowUs());

                mListener->issueCommand(
                    IStreamListener::DISCONTINUITY,
                    false,
                    extra);
            }

            size_t index = *mIndicesAvailable.begin();
            mIndicesAvailable.erase(mIndicesAvailable.begin());

            sp<IMemory> mem = mBuffers.itemAt(index);
            CHECK_LE(srcBuffer->size(), mem->size());
            CHECK_EQ((srcBuffer->size() % 188), 0u);

            memcpy(mem->pointer(), srcBuffer->data(), srcBuffer->size());
            mListener->queueBuffer(index, srcBuffer->size());
        }
    }


    FBoxRenderData::FBoxRenderData(
        const sp<AMessage> &notifyLost,
        const sp<IGraphicBufferProducer> &bufferProducer,
        const sp<AMessage> &msgNotify)
        : mNotifyLost(notifyLost),
          mBufferProducer(bufferProducer),
          mTotalBytesQueued(0ll),
          mMaxBytesQueued(0ll),
          mMinBytesQueued(0ll),
          mRetryTimes(0ll),
          mBytesQueued(0),
          mDebugEnable(false),
          mLastDequeuedExtSeqNo(-1),
          mFirstFailedAttemptUs(-1ll),
          mPackageSuccess(0),
          mPackageFailed(0),
          mPackageRequest(0),
          mRequestedRetry(false),
          mRequestedRetransmission(false),
          mMsgNotify(msgNotify),
          mIsHDCP(false)
    {
        mCurTime = ALooper::GetNowUs();

        int d = getPropertyInt("sys.wfddump", 0) ;
        d &= 0x02;
        if (d == 2)
            mDebugEnable = true;
        else
            mDebugEnable = false;
        ALOGE("Miracast debug info enable? d=%d, mDebugEnable =%d\n", d , mDebugEnable );
        if (mDebugEnable)
            ALOGE("Miracast debug info enabled\n");
        setProperty("sys.pkginfo", "suc:0,fail:0,req:0,total:0, max:0,min:0,retry:0,band:0");
        mIsDestoryState = false;
    }

    FBoxRenderData::~FBoxRenderData()
    {
        mIsDestoryState = true;
        destroyPlayer();
    }

    void FBoxRenderData::queueBuffer(const sp<ABuffer> &buffer)
    {
        Mutex::Autolock autoLock(mLock);
        int32_t value = 0;
        int32_t newExtendedSeqNo = buffer->int32Data();
        mTotalBytesQueued += buffer->size();
        mBytesQueued += buffer->size();
        mMaxBytesQueued = mMaxBytesQueued > mTotalBytesQueued ? mMaxBytesQueued : mTotalBytesQueued;
        if (mPackets.empty())
        {
            mPackets.push_back(buffer);
            return;
        }

        if (buffer->meta()->findInt32("seq_reset", &value)) {
            if (value) {
                ALOGE("Recieve seq_reset value is 0x%x 0x%x", value, newExtendedSeqNo);
                mPackets.push_back(buffer);
                return;
            }
        }

        List<sp<ABuffer> >::iterator firstIt = mPackets.begin();
        List<sp<ABuffer> >::iterator it = --mPackets.end();
        for (;;)
        {
            int32_t extendedSeqNo = (*it)->int32Data();

            if (extendedSeqNo == newExtendedSeqNo)
            {
                return;
            }

            if (extendedSeqNo < newExtendedSeqNo)
            {
                mPackets.insert(++it, buffer);
                return;
            }

            if (it == firstIt)
            {
                mPackets.insert(it, buffer);
                return;
            }

            --it;
        }
    }

    sp<ABuffer> FBoxRenderData::dequeueBuffer()
    {
        if (mIsDestoryState)
            return NULL;
        Mutex::Autolock autoLock(mLock);

        sp<ABuffer> buffer;
        int32_t extSeqNo = 0;
        int64_t curUs = 0;
        int32_t value = 0;

        while (!mPackets.empty())
        {
            value = 0;
            buffer = *mPackets.begin();
            extSeqNo = buffer->int32Data();
            if (buffer->meta()->findInt32("seq_reset", &value)) {
                if (value) {
                    ALOGE("Handle seq_reset valuse is 0x%x 0x%x", value, extSeqNo);
                    mLastDequeuedExtSeqNo = -1;
                    mRequestedRetransmission = false;
                    mRequestedRetry = false;
                    mFirstFailedAttemptUs = -1ll;
                }
            }
            if (mLastDequeuedExtSeqNo < 0 || extSeqNo > mLastDequeuedExtSeqNo)
            {
                break;
            }

            mTotalBytesQueued -= buffer->size();
            mMinBytesQueued = mMinBytesQueued < mTotalBytesQueued ? mMinBytesQueued : mTotalBytesQueued;
            buffer.clear();
            extSeqNo = -1;

            mPackets.erase(mPackets.begin());
        }

        if (mPackets.empty())
        {
            if (mFirstFailedAttemptUs < 0ll)
            {
                mFirstFailedAttemptUs = ALooper::GetNowUs();
                mRequestedRetry = false;
                mRequestedRetransmission = false;
            }
            else
            {
                float  noPacketTime = (ALooper::GetNowUs() - mFirstFailedAttemptUs) / 1E6;
                if (noPacketTime > 12.0) //beyond 12S
                {
                    int ret = -1;
                    ret = getPropertyInt("sys.wfd.state", 0);
                    if (ret == 0)
                    {
                        ALOGI("no packets available beyond 12 secs,stop WifiDisplaySink now");
                        sp<AMessage> notify = mMsgNotify->dup();
                        notify->setInt32("msg", kWhatNoPacketMsg);
                        notify->post();
                        mFirstFailedAttemptUs = ALooper::GetNowUs();
                    }
                    else if (ret == 2)
                    {
                        ALOGI("pause->play, reset noPacketTime!");
                        mFirstFailedAttemptUs = ALooper::GetNowUs();
                        setProperty("sys.wfd.state", "0");
                    }
                }
            }
            return NULL;
        }

        if (mLastDequeuedExtSeqNo < 0 || extSeqNo == mLastDequeuedExtSeqNo + 1)
        {
            if (mRequestedRetransmission)
            {
                ALOGE("Recovered after requesting retransmission of %d", extSeqNo);
            }

            if (!mRequestedRetry)
            {
                mPackageSuccess++;
            }
            else
            {
                mRequestedRetry = false;
                mPackageRequest++;
            }
            if (mDebugEnable)
            {
                curUs = ALooper::GetNowUs();
                if (curUs - mCurTime >= 1000000ll)
                {
                    mBandwidth = mBytesQueued * 1000000ll / (curUs - mCurTime);
                    mBytesQueued = 0;
                    mCurTime = ALooper::GetNowUs();
                }
            }
            mLastDequeuedExtSeqNo = extSeqNo;
            mFirstFailedAttemptUs = -1ll;
            mRequestedRetransmission = false;

            mPackets.erase(mPackets.begin());

            mTotalBytesQueued -= buffer->size();
            mMinBytesQueued = mMinBytesQueued < mTotalBytesQueued ? mMinBytesQueued : mTotalBytesQueued;
            return buffer;
        }
        if (mFirstFailedAttemptUs < 0ll) {
            mFirstFailedAttemptUs = ALooper::GetNowUs();
            ALOGI("failed to get the correct packet the first time.");
            return NULL;
        }
        if (!mRequestedRetransmission) {
            ALOGE("requesting retransmission of seqNo %d", (mLastDequeuedExtSeqNo + 1) & 0xffff);
            sp<AMessage> notify = mNotifyLost->dup();
            notify->setInt32("seqNo", (mLastDequeuedExtSeqNo + 1) & 0xffff);
            notify->post();
            mRequestedRetry = true;
            mRequestedRetransmission = true;
            mRetryTimes++;
        }
        if (mFirstFailedAttemptUs + 10000ll > ALooper::GetNowUs()) {
            return NULL;
        }
        ALOGI("dropping packet. extSeqNo %d didn't arrive in time", mLastDequeuedExtSeqNo + 1);
        mPackageFailed++;
        if (buffer->meta()->findInt32("seq_reordered", &value)) {
            if (value) {
                ALOGE("Handle seq_reordered extSeqNo %d %d mLastDequeuedExtSeqNo is %d %d",
                    extSeqNo, extSeqNo & 0xFFFF,  mLastDequeuedExtSeqNo, mLastDequeuedExtSeqNo & 0xFFFF);
                mLastDequeuedExtSeqNo++;
            }
        } else {
            mLastDequeuedExtSeqNo = extSeqNo;
        }
        mFirstFailedAttemptUs = -1ll;
        mRequestedRetransmission = false;
        mRequestedRetry = false;
        mTotalBytesQueued -= buffer->size();
        mMinBytesQueued = mMinBytesQueued < mTotalBytesQueued ? mMinBytesQueued : mTotalBytesQueued;
        mPackets.erase(mPackets.begin());
        return buffer;
    }

    void FBoxRenderData::onMessageReceived(const sp<AMessage> &msg)
    {
        switch (msg->what())
        {
        case kWhatQueueBuffer:
        {
            sp<ABuffer> buffer;
            CHECK(msg->findBuffer("buffer", &buffer));

            queueBuffer(buffer);

            if (mStreamSource == NULL)
            {
                if (mTotalBytesQueued > 0ll)
                {
                    initPlayer();
                }
            }
            else
            {
                mStreamSource->doSomeWork();
            }
            break;
        }

        default:
            TRESPASS();
        }
    }

    void FBoxRenderData::initPlayer()
    {
        if (mBufferProducer == NULL)
        {
            mComposerClient = new SurfaceComposerClient;
            CHECK_EQ(mComposerClient->initCheck(), (status_t)OK);

            DisplayInfo info;
            SurfaceComposerClient::getDisplayInfo(SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain), &info);
            ssize_t displayWidth = info.w;
            ssize_t displayHeight = info.h;

            mSurfaceControl =
                mComposerClient->createSurface(
                    String8("A Surface"),
                    displayWidth,
                    displayHeight,
                    PIXEL_FORMAT_RGB_565,
                    0);
            CHECK(mSurfaceControl != NULL);
            CHECK(mSurfaceControl->isValid());
            SurfaceComposerClient::openGlobalTransaction();
            CHECK_EQ(mSurfaceControl->setLayer(INT_MAX), (status_t)OK);
            CHECK_EQ(mSurfaceControl->show(), (status_t)OK);
            SurfaceComposerClient::closeGlobalTransaction();
            mSurface = mSurfaceControl->getSurface();
            CHECK(mSurface != NULL);
        }

        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder = sm->getService(String16("media.player"));
        sp<IMediaPlayerService> service = interface_cast<IMediaPlayerService>(binder);
        CHECK(service.get() != NULL);

        mStreamSource = new StreamSource(this);

        mPlayerClient = new PlayerClient;

        mPlayer = service->create( mPlayerClient/*, 0*/);
        CHECK(mPlayer != NULL);
        CHECK_EQ(mPlayer->setDataSource(sp<IStreamSource>(mStreamSource)), (status_t)OK);

        setProperty("media.libplayer.wfd", "1");
        setProperty("media.libplayer.fastswitch", "2");
        if (mIsHDCP)
        {
            ALOGI("HDCP Enabled!!!");
            Parcel data;
            data.writeInt32(1);
            mPlayer->setParameter(KEY_PARAMETER_AML_PLAYER_HDCP_CUSTOM_DATA, data);
        } else {
            ALOGI("HDCP is disabled!!!");
            Parcel data;
            data.writeInt32(0);
            mPlayer->setParameter(KEY_PARAMETER_AML_PLAYER_HDCP_CUSTOM_DATA, data);
        }
        mPlayer->setVideoSurfaceTexture(
                mBufferProducer != NULL ? mBufferProducer : mSurface->getIGraphicBufferProducer());
        Parcel request;
        mPlayer->prepareAsync();
        request.writeString16(String16("freerun_mode:60"));
        while (!mPlayerClient->isPrepared())
            usleep(1000);
        mPlayer->start();
    }

    void FBoxRenderData::destroyPlayer()
    {
        Parcel request;
        request.writeString16(String16("freerun_mode:0"));
        mPlayerClient.clear();
        mStreamSource.clear();
        if (mIsHDCP)
        {
            Parcel data;
            data.writeInt32(0);
        }
        setProperty("media.libplayer.wfd", "0");
        setProperty("media.libplayer.fastswitch", "");
        mPlayer->stop();
        mPlayer.clear();

        if (mBufferProducer == NULL)
        {
            mSurface.clear();
            mSurfaceControl.clear();

            mComposerClient->dispose();
            mComposerClient.clear();
        }
    }

    void FBoxRenderData::setIsHDCP(bool isHDCP)
    {
        mIsHDCP = isHDCP;
    }
}  // namespace android

