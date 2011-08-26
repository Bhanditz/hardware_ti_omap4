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
* @file OMXFocus.cpp
*
* This file contains functionality for handling focus configurations.
*
*/

#undef LOG_TAG

#define LOG_TAG "CameraHAL"

#include "CameraHal.h"
#include "OMXCameraAdapter.h"
#include "ErrorUtils.h"

#define TOUCH_FOCUS_RANGE 0xFF
#define AF_CALLBACK_TIMEOUT 10000000 //10 seconds timeout

namespace android {

status_t OMXCameraAdapter::setParametersFocus(const CameraParameters &params,
                                              BaseCameraAdapter::AdapterState state)
{
    status_t ret = NO_ERROR;
    const char *str = NULL;

    LOG_FUNCTION_NAME;

    str = params.get(CameraParameters::KEY_FOCUS_AREAS);
    mFocusAreas.clear();
    if ( NULL != str ) {
        ret = CameraArea::parseFocusArea(str, strlen(str), mFocusAreas);
    }

    if ( NO_ERROR == ret ) {
        if ( MAX_FOCUS_AREAS < mFocusAreas.size() ) {
            CAMHAL_LOGEB("Focus areas supported %d, focus areas set %d",
                         MAX_FOCUS_AREAS,
                         mFocusAreas.size());
            ret = -EINVAL;
        }
    }

    LOG_FUNCTION_NAME;

    return ret;
}

status_t OMXCameraAdapter::doAutoFocus()
{
    status_t ret = NO_ERROR;
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_IMAGE_CONFIG_FOCUSCONTROLTYPE focusControl;
    OMX_PARAM_FOCUSSTATUSTYPE focusStatus;

    LOG_FUNCTION_NAME;

    if ( OMX_StateExecuting != mComponentState )
        {
        CAMHAL_LOGEA("OMX component not in executing state");
        returnFocusStatus(false);
        return NO_INIT;
        }

    if ( 0 != mDoAFSem.Count() )
        {
        CAMHAL_LOGEB("Error mDoAFSem semaphore count %d", mDoAFSem.Count());
        return NO_INIT;
        }

    // If the app calls autoFocus, the camera will stop sending face callbacks.
    pauseFaceDetection(true);

    OMX_INIT_STRUCT_PTR (&focusControl, OMX_IMAGE_CONFIG_FOCUSCONTROLTYPE);
    focusControl.eFocusControl = ( OMX_IMAGE_FOCUSCONTROLTYPE ) mParameters3A.Focus;

    //In case we have CAF running we should first check the AF status.
    //If it has managed to lock, then do as usual and return status
    //immediately. If lock is not available, then switch temporarily
    //to 'autolock' and do normal AF.
    if ( mParameters3A.Focus == OMX_IMAGE_FocusControlAuto ) {
//FIXME: The CAF seems to return focus failure all the time.
// Probably this is tuning related, disable this until the
// MMS IQ team fixes it
#if 0
        ret = checkFocus(&focusStatus);
#else
        ret = NO_ERROR;
        focusStatus.eFocusStatus = OMX_FocusStatusReached;
#endif
        if ( NO_ERROR != ret ) {
            CAMHAL_LOGEB("Focus status check failed 0x%x!", ret);
            return ret;
        } else {
            CAMHAL_LOGDB("Focus status check 0x%x!", focusStatus.eFocusStatus);
        }

        if ( OMX_FocusStatusReached != focusStatus.eFocusStatus ) {
            focusControl.eFocusControl = OMX_IMAGE_FocusControlAutoLock;
        }
    }

    if ( ( focusControl.eFocusControl != OMX_IMAGE_FocusControlAuto ) &&
         ( focusControl.eFocusControl != ( OMX_IMAGE_FOCUSCONTROLTYPE )
                 OMX_IMAGE_FocusControlAutoInfinity ) ) {

        ret = RegisterForEvent(mCameraAdapterParameters.mHandleComp,
                                    (OMX_EVENTTYPE) OMX_EventIndexSettingChanged,
                                    OMX_ALL,
                                    OMX_IndexConfigCommonFocusStatus,
                                    mDoAFSem);

        if ( NO_ERROR == ret ) {
            ret = setFocusCallback(true);
        }

    }

    eError =  OMX_SetConfig(mCameraAdapterParameters.mHandleComp,
                            OMX_IndexConfigFocusControl,
                            &focusControl);

    if ( OMX_ErrorNone != eError ) {
        CAMHAL_LOGEB("Error while starting focus 0x%x", eError);
        return INVALID_OPERATION;
    } else {
        CAMHAL_LOGDA("Autofocus started successfully");
    }

    if ( ( focusControl.eFocusControl != OMX_IMAGE_FocusControlAuto ) &&
         ( focusControl.eFocusControl != ( OMX_IMAGE_FOCUSCONTROLTYPE )
                 OMX_IMAGE_FocusControlAutoInfinity ) ) {
        ret = mDoAFSem.WaitTimeout(AF_CALLBACK_TIMEOUT);
        //Disable auto focus callback from Ducati
        setFocusCallback(false);
        //Signal a dummy AF event so that in case the callback from ducati
        //does come then it doesnt crash after
        //exiting this function since eventSem will go out of scope.
        if(ret != NO_ERROR) {
            CAMHAL_LOGEA("Autofocus callback timeout expired");
            SignalEvent(mCameraAdapterParameters.mHandleComp,
                                        (OMX_EVENTTYPE) OMX_EventIndexSettingChanged,
                                        OMX_ALL,
                                        OMX_IndexConfigCommonFocusStatus,
                                        NULL );
            returnFocusStatus(true);
        } else {
            CAMHAL_LOGDA("Autofocus callback received");
            ret = returnFocusStatus(false);
        }

    } else {
        if ( NO_ERROR == ret ) {
            ret = returnFocusStatus(false);
        }
    }

    //Restore CAF if needed
    if ( ( mParameters3A.Focus == OMX_IMAGE_FocusControlAuto ) &&
         ( focusControl.eFocusControl == OMX_IMAGE_FocusControlAutoLock ) ) {
        mPending3Asettings |= SetFocus;
    }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::stopAutoFocus()
{
    status_t ret = NO_ERROR;
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_IMAGE_CONFIG_FOCUSCONTROLTYPE focusControl;

    LOG_FUNCTION_NAME;

    if ( OMX_StateExecuting != mComponentState )
        {
        CAMHAL_LOGEA("OMX component not in executing state");
        return NO_INIT;
        }

    if ( mParameters3A.Focus == OMX_IMAGE_FocusControlAutoInfinity ) {
        // No need to stop focus if we are in infinity mode. Nothing to stop.
        return NO_ERROR;
    }

    if ( NO_ERROR == ret )
       {
       //Disable the callback first
       ret = setFocusCallback(false);
       }

    if ( NO_ERROR == ret )
        {
        OMX_INIT_STRUCT_PTR (&focusControl, OMX_IMAGE_CONFIG_FOCUSCONTROLTYPE);
        focusControl.eFocusControl = OMX_IMAGE_FocusControlOff;

        eError =  OMX_SetConfig(mCameraAdapterParameters.mHandleComp,
                                OMX_IndexConfigFocusControl,
                                &focusControl);
        if ( OMX_ErrorNone != eError )
            {
            CAMHAL_LOGEB("Error while stopping focus 0x%x", eError);
            return ErrorUtils::omxToAndroidError(eError);
            }
        }

    //Query current focus distance after AF is complete
    updateFocusDistances(mParameters);

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::getFocusMode(OMX_IMAGE_CONFIG_FOCUSCONTROLTYPE &focusMode)
{;
    OMX_ERRORTYPE eError = OMX_ErrorNone;

    LOG_FUNCTION_NAME;

    if ( OMX_StateInvalid == mComponentState ) {
        CAMHAL_LOGEA("OMX component is in invalid state");
        return NO_INIT;
    }

    OMX_INIT_STRUCT_PTR (&focusMode, OMX_IMAGE_CONFIG_FOCUSCONTROLTYPE);
    focusMode.nPortIndex = mCameraAdapterParameters.mPrevPortIndex;

    eError =  OMX_GetConfig(mCameraAdapterParameters.mHandleComp,
                            OMX_IndexConfigFocusControl,
                            &focusMode);

    if ( OMX_ErrorNone != eError ) {
        CAMHAL_LOGEB("Error while retrieving focus mode 0x%x", eError);
    }

    LOG_FUNCTION_NAME_EXIT;

    return ErrorUtils::omxToAndroidError(eError);
}

status_t OMXCameraAdapter::cancelAutoFocus()
{
    status_t ret = NO_ERROR;
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_IMAGE_CONFIG_FOCUSCONTROLTYPE focusMode;

    LOG_FUNCTION_NAME;
    // Unlock 3A locks since they were locked by AF
    if( set3ALock(OMX_FALSE) != NO_ERROR) {
      CAMHAL_LOGEA("Error Unlocking 3A locks");
    }
    else{
      CAMHAL_LOGDA("AE/AWB unlocked successfully");
    }

    ret = getFocusMode(focusMode);
    if ( NO_ERROR != ret ) {
        return ret;
    }

    //Stop the AF only for modes other than CAF  or Inifinity
    if ( ( focusMode.eFocusControl != OMX_IMAGE_FocusControlAuto ) &&
         ( focusMode.eFocusControl != ( OMX_IMAGE_FOCUSCONTROLTYPE )
                 OMX_IMAGE_FocusControlAutoInfinity ) ) {
        stopAutoFocus();
        //Signal a dummy AF event so that in case the callback from ducati
        //does come then it doesnt crash after
        //exiting this function since eventSem will go out of scope.
        ret |= SignalEvent(mCameraAdapterParameters.mHandleComp,
                                    (OMX_EVENTTYPE) OMX_EventIndexSettingChanged,
                                    OMX_ALL,
                                    OMX_IndexConfigCommonFocusStatus,
                                    NULL );
    }

    // If the apps call #cancelAutoFocus()}, the face callbacks will also resume.
    pauseFaceDetection(false);

    LOG_FUNCTION_NAME_EXIT;

    return ret;

}

status_t OMXCameraAdapter::setFocusCallback(bool enabled)
{
    status_t ret = NO_ERROR;
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_CONFIG_CALLBACKREQUESTTYPE focusRequstCallback;

    LOG_FUNCTION_NAME;

    if ( OMX_StateExecuting != mComponentState )
        {
        CAMHAL_LOGEA("OMX component not in executing state");
        ret = -1;
        }

    if ( NO_ERROR == ret )
        {

        OMX_INIT_STRUCT_PTR (&focusRequstCallback, OMX_CONFIG_CALLBACKREQUESTTYPE);
        focusRequstCallback.nPortIndex = OMX_ALL;
        focusRequstCallback.nIndex = OMX_IndexConfigCommonFocusStatus;

        if ( enabled )
            {
            focusRequstCallback.bEnable = OMX_TRUE;
            }
        else
            {
            focusRequstCallback.bEnable = OMX_FALSE;
            }

        eError =  OMX_SetConfig(mCameraAdapterParameters.mHandleComp,
                                (OMX_INDEXTYPE) OMX_IndexConfigCallbackRequest,
                                &focusRequstCallback);
        if ( OMX_ErrorNone != eError )
            {
            CAMHAL_LOGEB("Error registering focus callback 0x%x", eError);
            ret = -1;
            }
        else
            {
            CAMHAL_LOGDB("Autofocus callback for index 0x%x registered successfully",
                         OMX_IndexConfigCommonFocusStatus);
            }
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::returnFocusStatus(bool timeoutReached)
{
    status_t ret = NO_ERROR;
    OMX_PARAM_FOCUSSTATUSTYPE eFocusStatus;
    bool focusStatus = false;
    BaseCameraAdapter::AdapterState state;
    BaseCameraAdapter::getState(state);

    LOG_FUNCTION_NAME;

    OMX_INIT_STRUCT(eFocusStatus, OMX_PARAM_FOCUSSTATUSTYPE);

    if( ( AF_ACTIVE & state ) != AF_ACTIVE )
       {
        /// We don't send focus callback if focus was not started
       return NO_ERROR;
       }

    if ( NO_ERROR == ret )
        {

        if ( !timeoutReached )
            {
            ret = checkFocus(&eFocusStatus);

            if ( NO_ERROR != ret )
                {
                CAMHAL_LOGEA("Focus status check failed!");
                }
            }
        }

    if ( NO_ERROR == ret )
        {

        if ( timeoutReached )
            {
            focusStatus = false;
            }
        ///FIXME: The ducati seems to return focus as false always if continuous focus is enabled
        ///So, return focus as locked always until this is fixed.
        else if(mParameters3A.Focus == OMX_IMAGE_FocusControlAuto )
            {
            focusStatus = true;
            }
        else
            {
            switch (eFocusStatus.eFocusStatus)
                {
                    case OMX_FocusStatusReached:
                        {
                        focusStatus = true;
                        //Lock the AE and AWB here sinc the focus is locked
                        // Apply 3A locks after AF
                        if( set3ALock(OMX_TRUE) != NO_ERROR) {
                            CAMHAL_LOGEA("Error Applying 3A locks");
                        }
                        else
                            {
                            CAMHAL_LOGDA("Focus locked. Applied focus locks successfully");
                            }
                        break;
                        }
                    case OMX_FocusStatusOff:
                    case OMX_FocusStatusUnableToReach:
                    case OMX_FocusStatusRequest:
                    default:
                        {
                        focusStatus = false;
                        break;
                        }
                }

            stopAutoFocus();
            }
        }

    ret =  BaseCameraAdapter::setState(CAMERA_CANCEL_AUTOFOCUS);
    if ( NO_ERROR == ret )
        {
        ret = BaseCameraAdapter::commitState();
        }
    else
        {
        ret |= BaseCameraAdapter::rollbackState();
        }

    if ( NO_ERROR == ret )
        {
        notifyFocusSubscribers(focusStatus);
        }

    // After focus, face detection will resume sending face callbacks
    pauseFaceDetection(false);

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::checkFocus(OMX_PARAM_FOCUSSTATUSTYPE *eFocusStatus)
{
    status_t ret = NO_ERROR;
    OMX_ERRORTYPE eError = OMX_ErrorNone;

    LOG_FUNCTION_NAME;

    if ( NULL == eFocusStatus )
        {
        CAMHAL_LOGEA("Invalid focus status");
        ret = -EINVAL;
        }

    if ( OMX_StateExecuting != mComponentState )
        {
        CAMHAL_LOGEA("OMX component not in executing state");
        ret = -EINVAL;
        }

    if ( NO_ERROR == ret )
        {
        OMX_INIT_STRUCT_PTR (eFocusStatus, OMX_PARAM_FOCUSSTATUSTYPE);

        eError = OMX_GetConfig(mCameraAdapterParameters.mHandleComp,
                               OMX_IndexConfigCommonFocusStatus,
                               eFocusStatus);
        if ( OMX_ErrorNone != eError )
            {
            CAMHAL_LOGEB("Error while retrieving focus status: 0x%x", eError);
            ret = -1;
            }
        }

    if ( NO_ERROR == ret )
        {
        CAMHAL_LOGDB("Focus Status: %d", eFocusStatus->eFocusStatus);
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::updateFocusDistances(CameraParameters &params)
{
    OMX_U32 focusNear, focusOptimal, focusFar;
    status_t ret = NO_ERROR;

    LOG_FUNCTION_NAME;

    ret = getFocusDistances(focusNear, focusOptimal, focusFar);
    if ( NO_ERROR == ret)
        {
        ret = addFocusDistances(focusNear, focusOptimal, focusFar, params);
            if ( NO_ERROR != ret )
                {
                CAMHAL_LOGEB("Error in call to addFocusDistances() 0x%x", ret);
                }
        }
    else
        {
        CAMHAL_LOGEB("Error in call to getFocusDistances() 0x%x", ret);
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::getFocusDistances(OMX_U32 &near,OMX_U32 &optimal, OMX_U32 &far)
{
    status_t ret = NO_ERROR;
    OMX_ERRORTYPE eError;

    OMX_TI_CONFIG_FOCUSDISTANCETYPE focusDist;

    LOG_FUNCTION_NAME;

    if ( OMX_StateInvalid == mComponentState )
        {
        CAMHAL_LOGEA("OMX component is in invalid state");
        ret = UNKNOWN_ERROR;
        }

    if ( NO_ERROR == ret )
        {
        OMX_INIT_STRUCT_PTR(&focusDist, OMX_TI_CONFIG_FOCUSDISTANCETYPE);
        focusDist.nPortIndex = mCameraAdapterParameters.mPrevPortIndex;

        eError = OMX_GetConfig(mCameraAdapterParameters.mHandleComp,
                               ( OMX_INDEXTYPE ) OMX_TI_IndexConfigFocusDistance,
                               &focusDist);
        if ( OMX_ErrorNone != eError )
            {
            CAMHAL_LOGEB("Error while querying focus distances 0x%x", eError);
            ret = UNKNOWN_ERROR;
            }

        }

    if ( NO_ERROR == ret )
        {
        near = focusDist.nFocusDistanceNear;
        optimal = focusDist.nFocusDistanceOptimal;
        far = focusDist.nFocusDistanceFar;
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::encodeFocusDistance(OMX_U32 dist, char *buffer, size_t length)
{
    status_t ret = NO_ERROR;
    uint32_t focusScale = 1000;
    float distFinal;

    LOG_FUNCTION_NAME;

    if(mParameters3A.Focus == OMX_IMAGE_FocusControlAutoInfinity)
        {
        dist=0;
        }

    if ( NO_ERROR == ret )
        {
        if ( 0 == dist )
            {
            strncpy(buffer, CameraParameters::FOCUS_DISTANCE_INFINITY, ( length - 1 ));
            }
        else
            {
            distFinal = dist;
            distFinal /= focusScale;
            snprintf(buffer, ( length - 1 ) , "%5.3f", distFinal);
            }
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::addFocusDistances(OMX_U32 &near,
                                             OMX_U32 &optimal,
                                             OMX_U32 &far,
                                             CameraParameters& params)
{
    status_t ret = NO_ERROR;

    LOG_FUNCTION_NAME;

    if ( NO_ERROR == ret )
        {
        ret = encodeFocusDistance(near, mFocusDistNear, FOCUS_DIST_SIZE);
        if ( NO_ERROR != ret )
            {
            CAMHAL_LOGEB("Error encoding near focus distance 0x%x", ret);
            }
        }

    if ( NO_ERROR == ret )
        {
        ret = encodeFocusDistance(optimal, mFocusDistOptimal, FOCUS_DIST_SIZE);
        if ( NO_ERROR != ret )
            {
            CAMHAL_LOGEB("Error encoding near focus distance 0x%x", ret);
            }
        }

    if ( NO_ERROR == ret )
        {
        ret = encodeFocusDistance(far, mFocusDistFar, FOCUS_DIST_SIZE);
        if ( NO_ERROR != ret )
            {
            CAMHAL_LOGEB("Error encoding near focus distance 0x%x", ret);
            }
        }

    if ( NO_ERROR == ret )
        {
        snprintf(mFocusDistBuffer, ( FOCUS_DIST_BUFFER_SIZE - 1) ,"%s,%s,%s", mFocusDistNear,
                                                                              mFocusDistOptimal,
                                                                              mFocusDistFar);

        params.set(CameraParameters::KEY_FOCUS_DISTANCES, mFocusDistBuffer);
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::setTouchFocus(size_t posX,
                                         size_t posY,
                                         size_t posWidth,
                                         size_t posHeight,
                                         size_t previewWidth,
                                         size_t previewHeight)
{
    status_t ret = NO_ERROR;
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_CONFIG_EXTFOCUSREGIONTYPE touchControl;

    LOG_FUNCTION_NAME;

    if ( OMX_StateInvalid == mComponentState )
        {
        CAMHAL_LOGEA("OMX component is in invalid state");
        ret = -1;
        }

    if ( NO_ERROR == ret )
        {
        OMX_INIT_STRUCT_PTR (&touchControl, OMX_CONFIG_EXTFOCUSREGIONTYPE);
        touchControl.nLeft = ( posX * TOUCH_FOCUS_RANGE ) / previewWidth;
        touchControl.nTop =  ( posY * TOUCH_FOCUS_RANGE ) / previewHeight;
        touchControl.nWidth = ( posWidth * TOUCH_FOCUS_RANGE ) / previewWidth;
        touchControl.nHeight = ( posHeight * TOUCH_FOCUS_RANGE ) / previewHeight;

        eError =  OMX_SetConfig(mCameraAdapterParameters.mHandleComp,
                                ( OMX_INDEXTYPE ) OMX_IndexConfigExtFocusRegion,
                                &touchControl);
        if ( OMX_ErrorNone != eError )
            {
            CAMHAL_LOGEB("Error while configuring touch focus 0x%x", eError);
            ret = -1;
            }
        else
            {
            CAMHAL_LOGDB("Touch focus %d,%d %d,%d configured successfuly",
                         ( int ) touchControl.nLeft,
                         ( int ) touchControl.nTop,
                         ( int ) touchControl.nWidth,
                         ( int ) touchControl.nHeight);
            }
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

};
