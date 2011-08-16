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
* @file OMXFD.cpp
*
* This file contains functionality for handling face detection.
*
*/

#undef LOG_TAG

#define LOG_TAG "CameraHAL"

#include "CameraHal.h"
#include "OMXCameraAdapter.h"

namespace android {

status_t OMXCameraAdapter::setParametersFD(const CameraParameters &params,
                                           BaseCameraAdapter::AdapterState state)
{
    status_t ret = NO_ERROR;

    LOG_FUNCTION_NAME;

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::startFaceDetection()
{
    Mutex::Autolock lock(mFaceDetectionLock);
    return setFaceDetection(true, mDeviceOrientation);
}

status_t OMXCameraAdapter::stopFaceDetection()
{
    Mutex::Autolock lock(mFaceDetectionLock);
    return setFaceDetection(false, mDeviceOrientation);
}

status_t OMXCameraAdapter::setFaceDetection(bool enable, OMX_U32 orientation)
{
    status_t ret = NO_ERROR;
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_CONFIG_EXTRADATATYPE extraDataControl;
    OMX_CONFIG_OBJDETECTIONTYPE objDetection;

    LOG_FUNCTION_NAME;

    if ( OMX_StateInvalid == mComponentState )
        {
        CAMHAL_LOGEA("OMX component is in invalid state");
        ret = -EINVAL;
        }

    // TODO(XXX): temporary hack. must remove after setconfig and transition issue
    //            with secondary camera is fixed
    if(mWaitToSetConfig) {
        if (enable) mFaceDetectionRunning = true;
        return NO_ERROR;
    }

    if ( NO_ERROR == ret )
        {
        if ( orientation < 0 || orientation > 270 ) {
            orientation = 0;
        }

        OMX_INIT_STRUCT_PTR (&objDetection, OMX_CONFIG_OBJDETECTIONTYPE);
        objDetection.nPortIndex = mCameraAdapterParameters.mPrevPortIndex;
        objDetection.nDeviceOrientation = orientation;
        if  ( enable )
            {
            objDetection.bEnable = OMX_TRUE;
            }
        else
            {
            objDetection.bEnable = OMX_FALSE;
            }

        eError =  OMX_SetConfig(mCameraAdapterParameters.mHandleComp,
                                ( OMX_INDEXTYPE ) OMX_IndexConfigImageFaceDetection,
                                &objDetection);
        if ( OMX_ErrorNone != eError )
            {
            CAMHAL_LOGEB("Error while configuring face detection 0x%x", eError);
            ret = -1;
            }
        else
            {
            CAMHAL_LOGDA("Face detection configured successfully");
            }
        }

    if ( NO_ERROR == ret )
        {
        OMX_INIT_STRUCT_PTR (&extraDataControl, OMX_CONFIG_EXTRADATATYPE);
        extraDataControl.nPortIndex = mCameraAdapterParameters.mPrevPortIndex;
        extraDataControl.eExtraDataType = OMX_FaceDetection;
        extraDataControl.eCameraView = OMX_2D;
        if  ( enable )
            {
            extraDataControl.bEnable = OMX_TRUE;
            }
        else
            {
            extraDataControl.bEnable = OMX_FALSE;
            }

        eError =  OMX_SetConfig(mCameraAdapterParameters.mHandleComp,
                                ( OMX_INDEXTYPE ) OMX_IndexConfigOtherExtraDataControl,
                                &extraDataControl);
        if ( OMX_ErrorNone != eError )
            {
            CAMHAL_LOGEB("Error while configuring face detection extra data 0x%x",
                         eError);
            ret = -1;
            }
        else
            {
            CAMHAL_LOGDA("Face detection extra data configured successfully");
            }
        }

    if ( NO_ERROR == ret )
        {
        mFaceDetectionRunning = enable;
        }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::detectFaces(OMX_BUFFERHEADERTYPE* pBuffHeader,
                                       sp<CameraFDResult> &result,
                                       size_t previewWidth,
                                       size_t previewHeight)
{
    status_t ret = NO_ERROR;
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_TI_FACERESULT *faceResult;
    OMX_OTHER_EXTRADATATYPE *extraData;
    OMX_FACEDETECTIONTYPE *faceData;
    OMX_TI_PLATFORMPRIVATE *platformPrivate;
    camera_frame_metadata_t *faces;

    LOG_FUNCTION_NAME;

    if ( OMX_StateExecuting != mComponentState ) {
        CAMHAL_LOGEA("OMX component is not in executing state");
        return NO_INIT;
    }

    if ( NULL == pBuffHeader ) {
        CAMHAL_LOGEA("Invalid Buffer header");
        return-EINVAL;
    }

    platformPrivate = (OMX_TI_PLATFORMPRIVATE *) (pBuffHeader->pPlatformPrivate);
    if ( NULL != platformPrivate ) {
        if ( sizeof(OMX_TI_PLATFORMPRIVATE) == platformPrivate->nSize ) {
            CAMHAL_LOGVB("Size = %d, sizeof = %d, pAuxBuf = 0x%x, pAuxBufSize= %d, pMetaDataBufer = 0x%x, nMetaDataSize = %d",
                         platformPrivate->nSize,
                         sizeof(OMX_TI_PLATFORMPRIVATE),
                         platformPrivate->pAuxBuf1,
                         platformPrivate->pAuxBufSize1,
                         platformPrivate->pMetaDataBuffer,
                         platformPrivate->nMetaDataSize);
        } else {
            CAMHAL_LOGEB("OMX_TI_PLATFORMPRIVATE size mismatch: expected = %d, received = %d",
                         ( unsigned int ) sizeof(OMX_TI_PLATFORMPRIVATE),
                         ( unsigned int ) platformPrivate->nSize);
            ret = -EINVAL;
        }
    }  else {
        CAMHAL_LOGEA("Invalid OMX_TI_PLATFORMPRIVATE");
        return-EINVAL;
    }


    if ( 0 >= platformPrivate->nMetaDataSize ) {
        CAMHAL_LOGEB("OMX_TI_PLATFORMPRIVATE nMetaDataSize is size is %d",
                     ( unsigned int ) platformPrivate->nMetaDataSize);
        return -EINVAL;
    }

    extraData = (OMX_OTHER_EXTRADATATYPE *) (platformPrivate->pMetaDataBuffer);
    if ( NULL != extraData ) {
        CAMHAL_LOGVB("Size = %d, sizeof = %d, eType = 0x%x, nDataSize= %d, nPortIndex = 0x%x, nVersion = 0x%x",
                     extraData->nSize,
                     sizeof(OMX_OTHER_EXTRADATATYPE),
                     extraData->eType,
                     extraData->nDataSize,
                     extraData->nPortIndex,
                     extraData->nVersion);
    } else {
        CAMHAL_LOGEA("Invalid OMX_OTHER_EXTRADATATYPE");
        return -EINVAL;
    }

    faceData = ( OMX_FACEDETECTIONTYPE * ) extraData->data;
    if ( NULL != faceData ) {
        if ( sizeof(OMX_FACEDETECTIONTYPE) == faceData->nSize ) {
            CAMHAL_LOGVB("Faces detected %d",
                         faceData->ulFaceCount,
                         faceData->nSize,
                         sizeof(OMX_FACEDETECTIONTYPE),
                         faceData->eCameraView,
                         faceData->nPortIndex,
                         faceData->nVersion);
        } else {
            CAMHAL_LOGDB("OMX_FACEDETECTIONTYPE size mismatch: expected = %d, received = %d",
                         ( unsigned int ) sizeof(OMX_FACEDETECTIONTYPE),
                         ( unsigned int ) faceData->nSize);
            return -EINVAL;
        }
    } else {
        CAMHAL_LOGEA("Invalid OMX_FACEDETECTIONTYPE");
        return -EINVAL;
    }

    ret = encodeFaceCoordinates(faceData, &faces, previewWidth, previewHeight);

    if ( NO_ERROR == ret ) {
        result = new CameraFDResult(faces);
    } else {
        result.clear();
        result = NULL;
    }

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

status_t OMXCameraAdapter::encodeFaceCoordinates(const OMX_FACEDETECTIONTYPE *faceData,
                                                 camera_frame_metadata_t **pFaces,
                                                 size_t previewWidth,
                                                 size_t previewHeight)
{
    status_t ret = NO_ERROR;
    camera_face_t *faces;
    camera_frame_metadata_t *faceResult;
    size_t hRange, vRange;
    double tmp;

    LOG_FUNCTION_NAME;

    if ( NULL == faceData ) {
        CAMHAL_LOGEA("Invalid OMX_FACEDETECTIONTYPE parameter");
        return EINVAL;
    }

    LOG_FUNCTION_NAME

    hRange = CameraFDResult::RIGHT - CameraFDResult::LEFT;
    vRange = CameraFDResult::BOTTOM - CameraFDResult::TOP;

    faceResult = ( camera_frame_metadata_t * ) malloc(sizeof(camera_frame_metadata_t));
    if ( NULL == faceResult ) {
        return -ENOMEM;
    }

    if ( 0 < faceData->ulFaceCount ) {
        int orient_mult;
        int trans_left, trans_top, trans_right, trans_bot;

        faces = ( camera_face_t * ) malloc(sizeof(camera_face_t)*faceData->ulFaceCount);
        if ( NULL == faces ) {
            return -ENOMEM;
        }

        /**
        / * When device is 180 degrees oriented to the sensor, need to translate
        / * the output from Ducati to what Android expects
        / * Ducati always gives face coordinates in this form, irrespective of
        / * rotation, i.e (l,t) always represents the point towards the left eye
        / * and top of hair.
        / * (l, t)
        / *   ---------------
        / *   -   ,,,,,,,   -
        / *   -  |       |  -
        / *   -  |<a   <a|  -
        / *   - (|   ^   |) -
        / *   -  |  -=-  |  -
        / *   -   \_____/   -
        / *   ---------------
        / *               (r, b)
        / *
        / * However, Android expects the coords to be in respect with what the
        / * sensor is viewing, i.e Android expects sensor to see this with (l,t)
        / * and (r,b) like so:
        / * (l, t)
        / *   ---------------
        / *   -    _____    -
        / *   -   /     \   -
        / *   -  |  -=-  |  -
        / *   - (|   ^   |) -
        / *   -  |a>   a>|  -
        / *   -  |       |  -
        / *   -   ,,,,,,,   -
        / *   ---------------
        / *               (r, b)
          */
        if (mDeviceOrientation == 180) {
            orient_mult = -1;
            trans_left = 2; // right is now left
            trans_top = 3; // bottom is now top
            trans_right = 0; // left is now right
            trans_bot = 1; // top is not bottom
        } else {
            orient_mult = 1;
            trans_left = 0; // left
            trans_top = 1; // top
            trans_right = 2; // right
            trans_bot = 3; // bottom

        }
        for ( int i = 0  ; i < faceData->ulFaceCount ; i++)
            {

            tmp = ( double ) faceData->tFacePosition[i].nLeft / ( double ) previewWidth;
            tmp *= hRange;
            tmp -= hRange/2;
            faces[i].rect[trans_left] = tmp;

            tmp = ( double ) faceData->tFacePosition[i].nTop / ( double )previewHeight;
            tmp *= vRange;
            tmp -= vRange/2;
            faces[i].rect[trans_top] = tmp;

            tmp = ( double ) faceData->tFacePosition[i].nWidth / ( double ) previewWidth;
            tmp *= hRange;
            tmp *= orient_mult;
            faces[i].rect[trans_right] = faces[i].rect[trans_left] + tmp;

            tmp = ( double ) faceData->tFacePosition[i].nHeight / ( double ) previewHeight;
            tmp *= vRange;
            tmp *= orient_mult;
            faces[i].rect[trans_bot] = faces[i].rect[trans_top] + tmp;

            faces[i].score = faceData->tFacePosition[i].nScore;
            faces[i].id = 0;
            faces[i].left_eye[0] = CameraFDResult::INVALID_DATA;
            faces[i].left_eye[1] = CameraFDResult::INVALID_DATA;
            faces[i].right_eye[0] = CameraFDResult::INVALID_DATA;
            faces[i].right_eye[1] = CameraFDResult::INVALID_DATA;
            faces[i].mouth[0] = CameraFDResult::INVALID_DATA;
            faces[i].mouth[1] = CameraFDResult::INVALID_DATA;
            }

        faceResult->number_of_faces = faceData->ulFaceCount;
        faceResult->faces = faces;

    } else {
        faceResult->number_of_faces = 0;
        faceResult->faces = NULL;
    }

    *pFaces = faceResult;

    LOG_FUNCTION_NAME_EXIT;

    return ret;
}

};
