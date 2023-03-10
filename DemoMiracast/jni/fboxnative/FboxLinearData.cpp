
#define LOG_NDEBUG 0
#define LOG_TAG "FboxLinearData"
#include <utils/Log.h>

#include "FboxLinearData.h"

#include <math.h>
#include <string.h>

namespace android
{

    FboxLinearData::FboxLinearData(size_t historySize)
        : mHistorySize(historySize),
          mCount(0),
          mHistory(new Point[mHistorySize]),
          mSumX(0.0),
          mSumY(0.0)
    {
    }

    FboxLinearData::~FboxLinearData()
    {
        delete[] mHistory;
        mHistory = NULL;
    }

    void FboxLinearData::addPoint(float x, float y)
    {
        if (mCount == mHistorySize)
        {
            const Point &oldest = mHistory[0];

            mSumX -= oldest.mX;
            mSumY -= oldest.mY;

            memmove(&mHistory[0], &mHistory[1], (mHistorySize - 1) * sizeof(Point));
            --mCount;
        }

        Point *newest = &mHistory[mCount++];
        newest->mX = x;
        newest->mY = y;

        mSumX += x;
        mSumY += y;
    }

    bool FboxLinearData::approxLine(float *n1, float *n2, float *b) const
    {
        static const float kEpsilon = 1.0E-4;

        if (mCount < 2)
        {
            return false;
        }

        float sumX2 = 0.0f;
        float sumY2 = 0.0f;
        float sumXY = 0.0f;

        float meanX = mSumX / (float)mCount;
        float meanY = mSumY / (float)mCount;

        for (size_t i = 0; i < mCount; ++i)
        {
            const Point &p = mHistory[i];

            float x = p.mX - meanX;
            float y = p.mY - meanY;

            sumX2 += x * x;
            sumY2 += y * y;
            sumXY += x * y;
        }

        float T = sumX2 + sumY2;
        float D = sumX2 * sumY2 - sumXY * sumXY;
        float root = sqrt(T * T * 0.25 - D);

        float L1 = T * 0.5 - root;

        if (fabs(sumXY) > kEpsilon)
        {
            *n1 = 1.0;
            *n2 = (2.0 * L1 - sumX2) / sumXY;

            float mag = sqrt((*n1) * (*n1) + (*n2) * (*n2));

            *n1 /= mag;
            *n2 /= mag;
        }
        else
        {
            *n1 = 0.0;
            *n2 = 1.0;
        }

        *b = (*n1) * meanX + (*n2) * meanY;

        return true;
    }

}  // namespace android

