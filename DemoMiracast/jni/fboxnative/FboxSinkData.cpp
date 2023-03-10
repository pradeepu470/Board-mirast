//DEVELOP BY PRADEEP 12/03/2021

#define LOG_NDEBUG 0
#define LOG_TAG "FboxSinkData"
#include <utils/Log.h>

#include "FboxSinkData.h"
#include "FboxUtils.h"

#include "FBoxNetworkSession.h"
#include "FBoxRenderData.h"
#include <binder/IServiceManager.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>

#include <media/stagefright/Utils.h>
namespace android
{

    struct FboxSinkData::Source : public RefBase
    {
        Source(uint16_t seq, const sp<ABuffer> &buffer,
               const sp<AMessage> queueBufferMsg);

        bool updateSeq(uint16_t seq, const sp<ABuffer> &buffer);

        void addReportBlock(uint32_t ssrc, const sp<ABuffer> &buf);

    protected:
        virtual ~Source();

    private:
        static const uint32_t kMinSequential = 2;
        static const uint32_t kMaxDropout = 3000;
        static const uint32_t kMaxMisorder = 100;
        static const uint32_t kRTPSeqMod = 1u << 16;

        sp<AMessage> mQueueBufferMsg;

        uint16_t mMaxSeq;
        uint32_t mCycles;
        uint32_t mBaseSeq;
        uint32_t mBadSeq;
        uint32_t mProbation;
        uint32_t mReceived;
        uint32_t mExpectedPrior;
        uint32_t mReceivedPrior;

        void initSeq(uint16_t seq);
        void queuePacket(const sp<ABuffer> &buffer);

        DISALLOW_EVIL_CONSTRUCTORS(Source);
    };

    FboxSinkData::Source::Source(
        uint16_t seq, const sp<ABuffer> &buffer,
        const sp<AMessage> queueBufferMsg)
        : mQueueBufferMsg(queueBufferMsg),
          mProbation(kMinSequential)
    {
        initSeq(seq);
        mMaxSeq = seq - 1;

        buffer->setInt32Data(mCycles | seq);
        queuePacket(buffer);
    }

    FboxSinkData::Source::~Source()
    {
    }

    void FboxSinkData::Source::initSeq(uint16_t seq)
    {
        mMaxSeq = seq;
        mCycles = 0;
        mBaseSeq = seq;
        mBadSeq = kRTPSeqMod + 1;
        mReceived = 0;
        mExpectedPrior = 0;
        mReceivedPrior = 0;
    }

    bool FboxSinkData::Source::updateSeq(uint16_t seq, const sp<ABuffer> &buffer)
    {
        uint16_t udelta = seq - mMaxSeq;

        if (mProbation)
        {

            if (seq == mMaxSeq + 1)
            {
                buffer->setInt32Data(mCycles | seq);
                queuePacket(buffer);

                --mProbation;
                mMaxSeq = seq;
                if (mProbation == 0)
                {
                    initSeq(seq);
                    ++mReceived;

                    return true;
                }
            }
            else
            {

                mProbation = kMinSequential - 1;
                mMaxSeq = seq;
                buffer->setInt32Data(mCycles | seq);
                queuePacket(buffer);
            }

            return false;
        }

        if (udelta < kMaxDropout)
        {

            if (seq < mMaxSeq)
            {
                mCycles += kRTPSeqMod;
                ALOGE("mCycles now set to mCycles %d", mCycles);
            }

            mMaxSeq = seq;
        }
        else if (udelta <= kRTPSeqMod - kMaxMisorder)
        {

            if (seq == mBadSeq)
            {
                initSeq(seq);
                ALOGE("very large jump seq is %d udelta is %d", seq, udelta);
                buffer->setInt32Data(mCycles | seq);
                buffer->meta()->setInt32("seq_reset", 1);
                ++mReceived;
                queuePacket(buffer);
                return true;
            }
            else
            {
                mBadSeq = (seq + 1) & (kRTPSeqMod - 1);
                return false;
            }
        }
        else
        {
            ALOGE("We may receive a duplicatr or reordered seq is %d udelta is %d", seq, udelta);
            buffer->meta()->setInt32("seq_reordered", 1);
        }
        ++mReceived;
        buffer->setInt32Data(mCycles | seq);
        queuePacket(buffer);
        return true;
    }

    void FboxSinkData::Source::queuePacket(const sp<ABuffer> &buffer)
    {
        sp<AMessage> msg = mQueueBufferMsg->dup();
        msg->setBuffer("buffer", buffer);
        msg->post();
    }

    void FboxSinkData::Source::addReportBlock(
        uint32_t ssrc, const sp<ABuffer> &buf)
    {
        uint32_t extMaxSeq = mMaxSeq | mCycles;
        uint32_t expected = extMaxSeq - mBaseSeq + 1;

        int64_t lost = (int64_t)expected - (int64_t)mReceived;
        if (lost > 0x7fffff)
        {
            lost = 0x7fffff;
        }
        else if (lost < -0x800000)
        {
            lost = -0x800000;
        }

        uint32_t expectedInterval = expected - mExpectedPrior;
        mExpectedPrior = expected;

        uint32_t receivedInterval = mReceived - mReceivedPrior;
        mReceivedPrior = mReceived;

        int64_t lostInterval = expectedInterval - receivedInterval;

        uint8_t fractionLost;
        if (expectedInterval == 0 || lostInterval <= 0)
        {
            fractionLost = 0;
        }
        else
        {
            fractionLost = (lostInterval << 8) / expectedInterval;
        }

        uint8_t *ptr = buf->data() + buf->size();

        ptr[0] = ssrc >> 24;
        ptr[1] = (ssrc >> 16) & 0xff;
        ptr[2] = (ssrc >> 8) & 0xff;
        ptr[3] = ssrc & 0xff;

        ptr[4] = fractionLost;

        ptr[5] = (lost >> 16) & 0xff;
        ptr[6] = (lost >> 8) & 0xff;
        ptr[7] = lost & 0xff;

        ptr[8] = extMaxSeq >> 24;
        ptr[9] = (extMaxSeq >> 16) & 0xff;
        ptr[10] = (extMaxSeq >> 8) & 0xff;
        ptr[11] = extMaxSeq & 0xff;

        ptr[12] = 0x00;  
        ptr[13] = 0x00;
        ptr[14] = 0x00;
        ptr[15] = 0x00;

        ptr[16] = 0x00; 
        ptr[17] = 0x00;
        ptr[18] = 0x00;
        ptr[19] = 0x00;

        ptr[20] = 0x00;
        ptr[21] = 0x00;
        ptr[22] = 0x00;
        ptr[23] = 0x00;
    }



    FboxSinkData::FboxSinkData(
        const sp<FBoxNetworkSession> &netSession,
        const sp<IGraphicBufferProducer> &bufferProducer,
        const sp<AMessage> &msgNotify)
        : mNetSession(netSession),
          mBufferProducer(bufferProducer),
          mRTPPort(0),
          mRTPSessionID(0),
          mRTCPSessionID(0),
          mFirstArrivalTimeUs(-1ll),
          mNumPacketsReceived(0ll),
          mRegression(1000),
          mMaxDelayMs(-1ll),
          mMsgNotify(msgNotify),
          mIsHDCP(false)
    {
        mDumpEnable = getPropertyInt("sys.wfddump", 0);
    }

    FboxSinkData::~FboxSinkData()
    {
        if (mRTCPSessionID != 0)
        {
            mNetSession->destroySession(mRTCPSessionID);
        }

        if (mRTPSessionID != 0)
        {
            mNetSession->destroySession(mRTPSessionID);
        }

        if (mRenderer != NULL)
        {
            looper()->unregisterHandler(mRenderer->id());
            mRenderer.clear();
        }
    }

    status_t FboxSinkData::init(bool useTCPInterleaving)
    {
        if (useTCPInterleaving)
        {
            return OK;
        }

        int clientRtp;

        sp<AMessage> rtpNotify = new AMessage(kWhatRTPNotify, this);
        sp<AMessage> rtcpNotify = new AMessage(kWhatRTCPNotify, this);
        for (clientRtp = 15550;; clientRtp += 2)
        {
            int32_t rtpSession;
            mNetSession->setRTPConnectionState(true);
            status_t err = mNetSession->createUDPSession(
                               clientRtp, rtpNotify, &rtpSession);

            if (err != OK)
            {
                ALOGI("failed to create RTP socket on port %d", clientRtp);
                continue;
            }

            mNetSession->setRTPConnectionState(false);

            int32_t rtcpSession;
            err = mNetSession->createUDPSession(
                      clientRtp + 1, rtcpNotify, &rtcpSession);

            if (err == OK)
            {
                mRTPPort = clientRtp;
                mRTPSessionID = rtpSession;
                mRTCPSessionID = rtcpSession;
                break;
            }

            ALOGI("failed to create RTCP socket on port %d", clientRtp + 1);
            mNetSession->destroySession(rtpSession);
        }

        if (mRTPPort == 0)
        {
            return UNKNOWN_ERROR;
        }

        return OK;
    }

    int32_t FboxSinkData::getRTPPort() const
    {
        return mRTPPort;
    }

    void FboxSinkData::onMessageReceived(const sp<AMessage> &msg)
    {
        switch (msg->what())
        {
        case kWhatRTPNotify:
        case kWhatRTCPNotify:
        {
            int32_t reason;
            CHECK(msg->findInt32("reason", &reason));

            switch (reason)
            {
            case FBoxNetworkSession::kWhatError:
            {
                int32_t sessionID;
                CHECK(msg->findInt32("sessionID", &sessionID));

                int32_t err;
                CHECK(msg->findInt32("err", &err));

                AString detail;
                CHECK(msg->findString("detail", &detail));

                ALOGE("An error occurred in session %d (%d, '%s/%s').",
                      sessionID,
                      err,
                      detail.c_str(),
                      strerror(-err));

                mNetSession->destroySession(sessionID);

                if (sessionID == mRTPSessionID)
                {
                    mRTPSessionID = 0;
                }
                else if (sessionID == mRTCPSessionID)
                {
                    mRTCPSessionID = 0;
                }
                break;
            }

            case FBoxNetworkSession::kWhatDatagram:
            {
                int32_t sessionID;
                CHECK(msg->findInt32("sessionID", &sessionID));

                sp<ABuffer> data;
                CHECK(msg->findBuffer("data", &data));

                status_t err;
                if (msg->what() == kWhatRTPNotify)
                {
                    ALOGV("kWhatRTPNotify: parseRTP");
                    err = parseRTP(data);
                }
                else
                {
                    ALOGV("kWhatRTCPNotify: parseRTCP");
                    err = parseRTCP(data);
                }
                break;
            }

            case FBoxNetworkSession::kWhatRTPConnect:
            {
                AString sourceHost;
                CHECK(msg->findString("fromAddr", &sourceHost));
                ALOGI("kWhatRTPConnect: %s", sourceHost.c_str());

                int32_t rtpprot;
                CHECK(msg->findInt32("fromPort", &rtpprot));
                ALOGI("kWhatRTPConnect: %d", rtpprot);
                if (msg->what() == kWhatRTPNotify)
                {
                    connect(sourceHost.c_str(), rtpprot, rtpprot + 1);
                }
                break;
            }

            default:
                TRESPASS();
            }
            break;
        }

        case kWhatSendRR:
        {
            onSendRR();
            break;
        }

        case kWhatPacketLost:
        {
            onPacketLost(msg);
            break;
        }

        case kWhatInject:
        {
            int32_t isRTP;
            CHECK(msg->findInt32("isRTP", &isRTP));

            sp<ABuffer> buffer;
            CHECK(msg->findBuffer("buffer", &buffer));

            status_t err;
            if (isRTP)
            {
                err = parseRTP(buffer);
            }
            else
            {
                err = parseRTCP(buffer);
            }
            break;
        }

        default:
            TRESPASS();
        }
    }

    status_t FboxSinkData::injectPacket(bool isRTP, const sp<ABuffer> &buffer)
    {
        sp<AMessage> msg = new AMessage(kWhatInject, this);
        msg->setInt32("isRTP", isRTP);
        msg->setBuffer("buffer", buffer);
        msg->post();

        return OK;
    }

    void FboxSinkData::setIsHDCP(bool isHDCP)
    {
        mIsHDCP = isHDCP;
    }

    static char dump_buf[2048];

    void dumpHex(unsigned char *buf, int len)
    {
        int i;
        int base;
        int offset = 0;
        int nwritten;
        for (offset = 0, base = 0; base < len; base += 16)
        {
            for (i = base; i < base + 16; i++)
            {
                if (i < len)
                {
                    nwritten = sprintf(dump_buf + offset , "%02x ", (unsigned) buf[i]);
                    offset += nwritten;
                }
                else
                {
                    nwritten = sprintf(dump_buf + offset , "   ");
                    offset += nwritten;
                }
            }
            nwritten = sprintf(dump_buf + offset , "\n");
            offset += nwritten;
        }

        ALOGI("%s", dump_buf);
    }



    static  FILE *fpo = NULL;

    int dump_to_file(char *filepath, char *data, int len)
    {
        if (fpo == NULL)
        {
            fpo = fopen(filepath, "w+");
            if (fpo == NULL)
            {
                ALOGI("failed to open output file %s", filepath);
                return -1;
            }
        }

        fwrite(data, 1, len, fpo);

        return 0;
    }


    status_t FboxSinkData::parseRTP(const sp<ABuffer> &buffer)
    {
        size_t size = buffer->size();
        if (size < 12)
        {
            return ERROR_MALFORMED;
        }

        const uint8_t *data = buffer->data();

        if ((data[0] >> 6) != 2)
        {
            return ERROR_UNSUPPORTED;
        }

        if (data[0] & 0x20)
        {

            size_t paddingLength = data[size - 1];

            if (paddingLength + 12 > size)
            {
                return ERROR_MALFORMED;
            }

            size -= paddingLength;
        }

        if (mDumpEnable == 1)
            dump_to_file((char *)"/data/misc/rtp.data", (char *)(buffer->data()) + 12, size - 12);
        int numCSRCs = data[0] & 0x0f;

        size_t payloadOffset = 12 + 4 * numCSRCs;

        if (size < payloadOffset)
        {
            return ERROR_MALFORMED;
        }

        if (data[0] & 0x10)
        {

            if (size < payloadOffset + 4)
            {

                return ERROR_MALFORMED;
            }

            const uint8_t *extensionData = &data[payloadOffset];

            size_t extensionLength =
                4 * (extensionData[2] << 8 | extensionData[3]);

            if (size < payloadOffset + 4 + extensionLength)
            {
                return ERROR_MALFORMED;
            }

            payloadOffset += 4 + extensionLength;
        }

        int64_t arrivalTimeUs;
        CHECK(buffer->meta()->findInt64("arrivalTimeUs", &arrivalTimeUs));

        if (mFirstArrivalTimeUs < 0ll)
        {
            mFirstArrivalTimeUs = arrivalTimeUs;
        }
        arrivalTimeUs -= mFirstArrivalTimeUs;

        int64_t arrivalTimeMedia = (arrivalTimeUs * 9ll) / 100ll;

        mRegression.addPoint((float)(U32_AT(&data[4])), (float)arrivalTimeMedia);

        ++mNumPacketsReceived;

        float n1, n2, b;
        if (mRegression.approxLine(&n1, &n2, &b))
        {

            float expectedArrivalTimeMedia = (b - n1 * (float)(U32_AT(&data[4]))) / n2;
            float latenessMs = (arrivalTimeMedia - expectedArrivalTimeMedia) / 90.0;

            if (mMaxDelayMs < 0ll || latenessMs > mMaxDelayMs)
            {
                mMaxDelayMs = latenessMs;
                ALOGV("packet was %.2f ms late", latenessMs);
            }
        }


        sp<AMessage> meta = buffer->meta();
        meta->setInt32("ssrc", U32_AT(&data[8]));
        meta->setInt32("rtp-time", U32_AT(&data[4]));
        meta->setInt32("PT", data[1] & 0x7f);
        meta->setInt32("M", data[1] >> 7);

        buffer->setRange(payloadOffset, size - payloadOffset);

        ssize_t index = mSources.indexOfKey(U32_AT(&data[8]));
        if (index < 0)
        {
            if (mRenderer == NULL)
            {
                sp<AMessage> notifyLost = new AMessage(kWhatPacketLost, this);
                notifyLost->setInt32("ssrc", U32_AT(&data[8]));

                mRenderer = new FBoxRenderData(notifyLost, mBufferProducer, mMsgNotify);
                looper()->registerHandler(mRenderer);

                mRenderer->setIsHDCP(mIsHDCP);
            }

            sp<AMessage> queueBufferMsg =
                new AMessage(FBoxRenderData::kWhatQueueBuffer, mRenderer);

            sp<Source> source = new Source(U16_AT(&data[2]), buffer, queueBufferMsg);
            mSources.add(U32_AT(&data[8]), source);
        }
        else
        {
            mSources.valueAt(index)->updateSeq(U16_AT(&data[2]), buffer);
        }

        return OK;
    }

    status_t FboxSinkData::parseRTCP(const sp<ABuffer> &buffer)
    {
        const uint8_t *data = buffer->data();
        size_t size = buffer->size();

        while (size > 0)
        {
            if (size < 8)
            {
                return ERROR_MALFORMED;
            }

            if ((data[0] >> 6) != 2)
            {
                return ERROR_UNSUPPORTED;
            }

            if (data[0] & 0x20)
            {

                size_t paddingLength = data[size - 1];

                if (paddingLength + 12 > size)
                {
                    return ERROR_MALFORMED;
                }

                size -= paddingLength;
            }

            size_t headerLength = 4 * (data[2] << 8 | data[3]) + 4;

            if (size < headerLength)
            {
                return ERROR_MALFORMED;
            }

            switch (data[1])
            {
            case 200:
            {
                parseSR(data, headerLength);
                break;
            }

            case 201: 
            case 202:  
            case 204:  
            case 205:  
            case 206:  
                break;

            case 203:
            {
                parseBYE(data, headerLength);
                break;
            }

            default:
            {
                break;
            }
            }

            data += headerLength;
            size -= headerLength;
        }

        return OK;
    }

    status_t FboxSinkData::parseBYE(const uint8_t *data, size_t size)
    {
        size_t SC = data[0] & 0x3f;

        if (SC == 0 || size < (4 + SC * 4))
        {
            return ERROR_MALFORMED;
        }

        return OK;
    }

    status_t FboxSinkData::parseSR(const uint8_t *data, size_t size)
    {
        size_t RC = data[0] & 0x1f;

        if (size < (7 + RC * 6) * 4)
        {
            return ERROR_MALFORMED;
        }

        return OK;
    }

    status_t FboxSinkData::connect(
        const char *host, int32_t remoteRtpPort, int32_t remoteRtcpPort)
    {
        ALOGI("connecting RTP/RTCP sockets to %s:{%d,%d}",
              host, remoteRtpPort, remoteRtcpPort);

        status_t err =
            mNetSession->connectUDPSession(mRTPSessionID, host, remoteRtpPort);

        if (err != OK)
        {
            return err;
        }

        err = mNetSession->connectUDPSession(mRTCPSessionID, host, remoteRtcpPort);

        if (err != OK)
        {
            return err;
        }

        return OK;
    }

    void FboxSinkData::scheduleSendRR()
    {
        (new AMessage(kWhatSendRR, this))->post(2000000ll);
    }

    void FboxSinkData::addSDES(const sp<ABuffer> &buffer)
    {
        uint8_t *data = buffer->data() + buffer->size();
        data[0] = 0x80 | 1;
        data[1] = 202;  // SDES
        data[4] = 0xde;  // SSRC
        data[5] = 0xad;
        data[6] = 0xbe;
        data[7] = 0xef;

        size_t offset = 8;

        data[offset++] = 1;  // CNAME

        AString cname = "stagefright@somewhere";
        data[offset++] = cname.size();

        memcpy(&data[offset], cname.c_str(), cname.size());
        offset += cname.size();

        data[offset++] = 6;  // TOOL

        AString tool = "stagefright/1.0";
        data[offset++] = tool.size();

        memcpy(&data[offset], tool.c_str(), tool.size());
        offset += tool.size();

        data[offset++] = 0;

        if ((offset % 4) > 0)
        {
            size_t count = 4 - (offset % 4);
            switch (count)
            {
            case 3:
                data[offset++] = 0;
            case 2:
                data[offset++] = 0;
            case 1:
                data[offset++] = 0;
            }
        }

        size_t numWords = (offset / 4) - 1;
        data[2] = numWords >> 8;
        data[3] = numWords & 0xff;

        buffer->setRange(buffer->offset(), buffer->size() + offset);
    }

    void FboxSinkData::onSendRR()
    {
        sp<ABuffer> buf = new ABuffer(1500);
        buf->setRange(0, 0);

        uint8_t *ptr = buf->data();
        ptr[0] = 0x80 | 0;
        ptr[1] = 201;  // RR
        ptr[2] = 0;
        ptr[3] = 1;
        ptr[4] = 0xde;  // SSRC
        ptr[5] = 0xad;
        ptr[6] = 0xbe;
        ptr[7] = 0xef;

        buf->setRange(0, 8);

        size_t numReportBlocks = 0;
        for (size_t i = 0; i < mSources.size(); ++i)
        {
            uint32_t ssrc = mSources.keyAt(i);
            sp<Source> source = mSources.valueAt(i);

            if (numReportBlocks > 31 || buf->size() + 24 > buf->capacity())
            {
                break;
            }

            source->addReportBlock(ssrc, buf);
            ++numReportBlocks;
        }

        ptr[0] |= numReportBlocks;  // 5 bit

        size_t sizeInWordsMinus1 = 1 + 6 * numReportBlocks;
        ptr[2] = sizeInWordsMinus1 >> 8;
        ptr[3] = sizeInWordsMinus1 & 0xff;

        buf->setRange(0, (sizeInWordsMinus1 + 1) * 4);

        addSDES(buf);

        ALOGE("Send RTCP Receiver Report");
        mNetSession->sendRequest(mRTCPSessionID, buf->data(), buf->size());

        scheduleSendRR();
    }

    void FboxSinkData::onPacketLost(const sp<AMessage> &msg)
    {
        uint32_t srcId;
        CHECK(msg->findInt32("ssrc", (int32_t *)&srcId));

        int32_t seqNo;
        CHECK(msg->findInt32("seqNo", &seqNo));

        int32_t blp = 0;

        sp<ABuffer> buf = new ABuffer(1500);
        buf->setRange(0, 0);

        uint8_t *ptr = buf->data();
        ptr[0] = 0x80 | 1;  // generic NACK
        ptr[1] = 205;  // RTPFB
        ptr[2] = 0;
        ptr[3] = 3;
        ptr[4] = 0xde;  // sender SSRC
        ptr[5] = 0xad;
        ptr[6] = 0xbe;
        ptr[7] = 0xef;
        ptr[8] = (srcId >> 24) & 0xff;
        ptr[9] = (srcId >> 16) & 0xff;
        ptr[10] = (srcId >> 8) & 0xff;
        ptr[11] = (srcId & 0xff);
        ptr[12] = (seqNo >> 8) & 0xff;
        ptr[13] = (seqNo & 0xff);
        ptr[14] = (blp >> 8) & 0xff;
        ptr[15] = (blp & 0xff);

        buf->setRange(0, 16);

        mNetSession->sendRequest(mRTCPSessionID, buf->data(), buf->size());
    }

}  // namespace android

