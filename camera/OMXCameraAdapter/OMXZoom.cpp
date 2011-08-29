/*
 * Copyright (C) Texas Instruments - http://www.ti.com/
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
* @file OMXZoom.cpp
*
* This file contains functionality for handling zoom configurations.
*
*/

#undef LOG_TAG

#define LOG_TAG "CameraHAL"

#include "CameraHal.h"
#include "OMXCameraAdapter.h"

namespace android {

const int32_t OMXCameraAdapter::ZOOM_STEPS [ZOOM_STAGES] =  {
                                65536, 70124,
                                75366, 80609,
                                86508, 92406,
                                99615,  106168,
                                114033, 122552,
                                131072, 140247,
                                150733, 161219,
                                173015, 185467,
                                198574, 212992,
                                228065, 244449,
                                262144, 281149,
                                300810, 322437,
                                346030, 370934,
                                397148, 425984,
                                456131, 488899,
                                524288 };


status_t OMXCameraAdapter::setParametersZoom(const CameraParameters &params,
                                             BaseCameraAdapter::AdapterState state)
{
    status_t ret = NO_ERROR;
    Mutex::Autolock lock(mZoomLock);

    LOG_FUNCTION_NAME;

    //Immediate zoom should not be avaialable while smooth zoom is running
    if ( ( ZOOM_ACTIVE & state ) != ZOOM_ACTIVE )
        {
        int zoom = params.getInt(CameraParameters::KEY_ZOOM);
        if( ( zoom >= 0 ) && ( zoom < ZOOM_STAGES ) )
            {
            mTargetZoomIdx = zoom;

            //Immediate zoom should be applied instantly ( CTS requirement )
            mCurrentZoomIdx = mTargetZoomIdx;
            doZoom(mCurrentZoomIdx);

            CAMHAL_LOGDB("Zoom by App %d", zoom);
            }
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::doZoom(int index)
{
    status_t ret = NO_ERROR;
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_CONFIG_SCALEFACTORTYPE zoomControl;
    static int prevIndex = 0;

    LOG_FUNCTION_NAME;

    if ( OMX_StateInvalid == mComponentState )
        {
        CAMHAL_LOGEA("OMX component is in invalid state");
        ret = -1;
        }

    if (  ( 0 > index) || ( ( ZOOM_STAGES - 1 ) < index ) )
        {
        CAMHAL_LOGEB("Zoom index %d out of range", index);
        ret = -EINVAL;
        }

    if ( prevIndex == index )
        {
        return NO_ERROR;
        }

    if ( NO_ERROR == ret )
        {
        OMX_INIT_STRUCT_PTR (&zoomControl, OMX_CONFIG_SCALEFACTORTYPE);
        zoomControl.nPortIndex = OMX_ALL;
        zoomControl.xHeight = ZOOM_STEPS[index];
        zoomControl.xWidth = ZOOM_STEPS[index];

        eError =  OMX_SetConfig(mCameraAdapterParameters.mHandleComp,
                                OMX_IndexConfigCommonDigitalZoom,
                                &zoomControl);
        if ( OMX_ErrorNone != eError )
            {
            CAMHAL_LOGEB("Error while applying digital zoom 0x%x", eError);
            ret = -1;
            }
        else
            {
            CAMHAL_LOGDA("Digital zoom applied successfully");
            prevIndex = index;
            }
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::advanceZoom()
{
    status_t ret = NO_ERROR;
    AdapterState state;
    BaseCameraAdapter::getState(state);

    if ( mReturnZoomStatus )
        {
        mCurrentZoomIdx +=mZoomInc;
        mTargetZoomIdx = mCurrentZoomIdx;
        mReturnZoomStatus = false;
        ret = doZoom(mCurrentZoomIdx);
        notifyZoomSubscribers(mCurrentZoomIdx, true);
        }
    else if ( mCurrentZoomIdx != mTargetZoomIdx )
        {
        if ( ZOOM_ACTIVE & state )
            {
            if ( mCurrentZoomIdx < mTargetZoomIdx )
                {
                mZoomInc = 1;
                }
            else
                {
                mZoomInc = -1;
                }

            mCurrentZoomIdx += mZoomInc;
            }
        else
            {
            mCurrentZoomIdx = mTargetZoomIdx;
            }

        ret = doZoom(mCurrentZoomIdx);

        if ( ZOOM_ACTIVE & state )
            {
            if ( mCurrentZoomIdx == mTargetZoomIdx )
                {
                CAMHAL_LOGDB("[Goal Reached] Smooth Zoom notify currentIdx = %d, targetIdx = %d",
                             mCurrentZoomIdx,
                             mTargetZoomIdx);

                if ( NO_ERROR == ret )
                    {

                    ret =  BaseCameraAdapter::setState(CAMERA_STOP_SMOOTH_ZOOM);

                    if ( NO_ERROR == ret )
                        {
                        ret = BaseCameraAdapter::commitState();
                        }
                    else
                        {
                        ret |= BaseCameraAdapter::rollbackState();
                        }

                    }
                mReturnZoomStatus = false;
                notifyZoomSubscribers(mCurrentZoomIdx, true);
                }
            else
                {
                CAMHAL_LOGDB("[Advancing] Smooth Zoom notify currentIdx = %d, targetIdx = %d",
                             mCurrentZoomIdx,
                             mTargetZoomIdx);
                notifyZoomSubscribers(mCurrentZoomIdx, false);
                }
            }
        }
    else if ( (mCurrentZoomIdx == mTargetZoomIdx ) &&
              ( ZOOM_ACTIVE & state ) )
        {
        ret = BaseCameraAdapter::setState(CameraAdapter::CAMERA_STOP_SMOOTH_ZOOM);

        if ( NO_ERROR == ret )
            {
            ret = BaseCameraAdapter::commitState();
            }
        else
            {
            ret |= BaseCameraAdapter::rollbackState();
            }
        }

    return ret;
}

status_t OMXCameraAdapter::startSmoothZoom(int targetIdx)
{
    status_t ret = NO_ERROR;

    LOG_FUNCTION_NAME;

    Mutex::Autolock lock(mZoomLock);

    CAMHAL_LOGDB("Start smooth zoom target = %d, mCurrentIdx = %d",
                 targetIdx,
                 mCurrentZoomIdx);

    if ( ( targetIdx >= 0 ) && ( targetIdx < ZOOM_STAGES ) )
        {
        mTargetZoomIdx = targetIdx;
        mZoomParameterIdx = mCurrentZoomIdx;
        mReturnZoomStatus = false;
        }
    else
        {
        CAMHAL_LOGEB("Smooth value out of range %d!", targetIdx);
        ret = -EINVAL;
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::stopSmoothZoom()
{
    status_t ret = NO_ERROR;
    Mutex::Autolock lock(mZoomLock);

    LOG_FUNCTION_NAME;

    if ( mTargetZoomIdx != mCurrentZoomIdx )
        {
        if ( mCurrentZoomIdx < mTargetZoomIdx )
            {
            mZoomInc = 1;
            }
        else
            {
            mZoomInc = -1;
            }
        mReturnZoomStatus = true;
        mReturnZoomStatus = true;
        CAMHAL_LOGDB("Stop smooth zoom mCurrentZoomIdx = %d, mTargetZoomIdx = %d",
                     mCurrentZoomIdx,
                     mTargetZoomIdx);
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

};
