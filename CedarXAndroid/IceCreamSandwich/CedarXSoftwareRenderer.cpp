/*
 * Copyright (C) 2009 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "CedarXSoftwareRenderer"
#include <CDX_Debug.h>

#include <binder/MemoryHeapBase.h>

#if (CEDARX_ANDROID_VERSION < 7)
#include <binder/MemoryHeapPmem.h>
#include <surfaceflinger/Surface.h>
#include <ui/android_native_buffer.h>
#else
#include <system/window.h>
#endif

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MetaData.h>
#include <ui/GraphicBufferMapper.h>
#include <gui/ISurfaceTexture.h>

#include "CedarXSoftwareRenderer.h"

extern "C" {
extern unsigned int cedarv_address_phy2vir(void *addr);
}
namespace android {

CedarXSoftwareRenderer::CedarXSoftwareRenderer(
        const sp<ANativeWindow> &nativeWindow, const sp<MetaData> &meta)
    : mYUVMode(None),
      mNativeWindow(nativeWindow) {
    int32_t tmp;
    CHECK(meta->findInt32(kKeyColorFormat, &tmp));
    //mColorFormat = (OMX_COLOR_FORMATTYPE)tmp;

    //CHECK(meta->findInt32(kKeyScreenID, &screenID));
    //CHECK(meta->findInt32(kKeyColorFormat, &halFormat));
    CHECK(meta->findInt32(kKeyWidth, &mWidth));
    CHECK(meta->findInt32(kKeyHeight, &mHeight));

    int32_t rotationDegrees;
    if (!meta->findInt32(kKeyRotation, &rotationDegrees)) {
        rotationDegrees = 0;
    }

    int halFormat;
    size_t bufWidth, bufHeight;

    halFormat = HAL_PIXEL_FORMAT_YV12;
    bufWidth = mWidth;
    bufHeight = mHeight;
    if(bufWidth != ((mWidth + 15) & ~15))
    {
        LOGW("(f:%s, l:%d) bufWidth[%d]!=display_width[%d]", __FUNCTION__, __LINE__, ((mWidth + 15) & ~15), mWidth);
    }
    if(bufHeight != ((mHeight + 15) & ~15))
    {
        LOGW("(f:%s, l:%d) bufHeight[%d]!=display_height[%d]", __FUNCTION__, __LINE__, ((mHeight + 15) & ~15), mHeight);
    }

    CHECK(mNativeWindow != NULL);

    CHECK_EQ(0,
            native_window_set_usage(
            mNativeWindow.get(),
            GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_SW_WRITE_OFTEN
            | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP));

//    CHECK_EQ(0,
//            native_window_set_scaling_mode(
//            mNativeWindow.get(),
//            NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW));

    // Width must be multiple of 32??? 
    CHECK_EQ(0, native_window_set_buffers_geometry(
                mNativeWindow.get(),
                //(bufWidth + 15) & ~15,
                //(bufHeight+ 15) & ~15,
                (bufWidth + 15) & ~15,
                bufHeight,
                //bufWidth,
                //bufHeight,
                halFormat));
    uint32_t transform;
    switch (rotationDegrees) {
        case 0: transform = 0; break;
        case 90: transform = HAL_TRANSFORM_ROT_90; break;
        case 180: transform = HAL_TRANSFORM_ROT_180; break;
        case 270: transform = HAL_TRANSFORM_ROT_270; break;
        default: transform = 0; break;
    }

    if (transform) {
        CHECK_EQ(0, native_window_set_buffers_transform(
                    mNativeWindow.get(), transform));
    }
    Rect crop;
    crop.left = 0;
    crop.top  = 0;
    crop.right = bufWidth;
    crop.bottom = bufHeight;
    mNativeWindow->perform(mNativeWindow.get(), NATIVE_WINDOW_SET_CROP, &crop);
}

CedarXSoftwareRenderer::~CedarXSoftwareRenderer() {
}

static int ALIGN(int x, int y) {
    // y must be a power of 2.
    return (x + y - 1) & ~(y - 1);
}

void CedarXSoftwareRenderer::render0(const void *data, size_t size, void *platformPrivate)
{
    ANativeWindowBuffer *buf;
	libhwclayerpara_t*  pOverlayParam;
    int err;
    if ((err = mNativeWindow->dequeueBuffer_DEPRECATED(mNativeWindow.get(), &buf)) != 0) {
        LOGW("Surface::dequeueBuffer returned error %d", err);
        return;
    }
    CHECK_EQ(0, mNativeWindow->lockBuffer_DEPRECATED(mNativeWindow.get(), buf));

    GraphicBufferMapper &mapper = GraphicBufferMapper::get();
    
    
    //a10_GPUbuffer_YV12, Y:16*2 align, UV:16*1 align.
    //a10_vdecbuffer_YV12, Y:16*16 align, UV:16*8 align.
    //a31_GPUbuffer_YV12, Y:32*2 align, UV:16*1 align.
    //a31_vdecbuffer_YV12, Y:16*16 align, UV:16*8 or 8*8 align

    //we make the rule: the buffersize request from GPU is Y:16*16 align!
    //But in A31, gpu buffer is 32align in width at least, 
    //so the requested a31_gpu_buffer is Y_32*16align, uv_16*8 actually.
    
    //Rect bounds((mWidth+15)&~15, (mHeight+15)&~15); 
    Rect bounds((mWidth+15)&~15, (mHeight+15)&~15);
    //Rect bounds(mWidth, mHeight);

    void *dst;
    CHECK_EQ(0, mapper.lock(buf->handle, GRALLOC_USAGE_SW_WRITE_OFTEN, bounds, &dst));

    //LOGD("buf->stride:%d, buf->width:%d, buf->height:%d buf->format:%d, buf->usage:%d,WXH:%dx%d dst:%p", 
    //    buf->stride, buf->width, buf->height, buf->format, buf->usage, mWidth, mHeight, dst);
    
    //LOGV("mColorFormat: %d", mColorFormat);
#if defined(__CHIP_VERSION_F23)
    size_t dst_y_size = buf->stride * buf->height;
    size_t dst_c_stride = ALIGN(buf->stride / 2, 16);    //16
    size_t dst_c_size = dst_c_stride * buf->height / 2;

    int src_y_width = (mWidth+15)&~15;
    int src_y_height = (mHeight+15)&~15;
    int src_c_width = ALIGN(src_y_width / 2, 16);   //uv_width_align=16
    int src_c_height = src_y_height/2;
    int src_display_y_width = mWidth;
    int src_display_y_height = mHeight;
    int src_display_c_width = src_display_y_width/2;
    int src_display_c_height = src_display_y_height/2;
    
    size_t src_display_y_size = src_y_width * src_display_y_height;
    size_t src_display_c_size = src_c_width * src_display_c_height;
    size_t src_y_size = src_y_width * src_y_height;
    size_t src_c_size = src_c_width * src_c_height;
    //LOGV("buf->stride:%d buf->height:%d WXH:%dx%d dst:%p data:%p", buf->stride, buf->height, mWidth, mHeight, dst, data);
    //copy_method_1:
    memcpy(dst, data, dst_y_size + dst_c_size*2);
    //copy_method_2:
    //memcpy(dst, data, src_y_size + src_c_size*2);
    //copy_method_3:
//    memcpy(dst, data, src_display_y_size);
//    dst += dst_y_size;
//    data += src_y_size;
//    memcpy(dst, data, src_display_c_size);
//    dst += dst_c_size;
//    data += src_c_size;
//    memcpy(dst, data, src_display_c_size);
#elif defined(__CHIP_VERSION_F33)
        pOverlayParam = (libhwclayerpara_t*)data;

//    LOGD("buf->stride:%d buf->height:%d WXH:%dx%d cstride:%d", buf->stride, buf->height, mWidth, mHeight, dst_c_stride);
//    {
//    	struct timeval t;
//    	int64_t startTime;
//    	int64_t endTime;
//
//    	gettimeofday(&t, NULL);
//    	startTime = (int64_t)t.tv_sec*1000000 + t.tv_usec;
    {
    	int i;
    	int widthAlign;
    	int heightAlign;
    	int cHeight;
    	int cWidth;
    	int dstCStride;

    	unsigned char* dstPtr;
    	unsigned char* srcPtr;
    	dstPtr = (unsigned char*)dst;
    	srcPtr = (unsigned char*)cedarv_address_phy2vir((void*)pOverlayParam->addr[0]);
    	widthAlign = (mWidth + 15) & ~15;
    	heightAlign = (mHeight + 15) & ~15;
    	for(i=0; i<heightAlign; i++)
    	{
    		memcpy(dstPtr, srcPtr, widthAlign);
    		dstPtr += buf->stride;
    		srcPtr += widthAlign;
    	}

    	cWidth = (mWidth/2 + 15) & ~15;
    	cHeight = heightAlign;
    	for(i=0; i<cHeight; i++)
    	{
    		memcpy(dstPtr, srcPtr, cWidth);
    		dstPtr += cWidth;
    		srcPtr += cWidth;
    	}
    }
//        {
//        	int height_align;
//        	int width_align;
//        	height_align = (buf->height+15) & ~15;
//        	memcpy(dst, (void*)pOverlayParam->addr[0], dst_y_size);
//        	memcpy((unsigned char*)dst + dst_y_size, (void*)(pOverlayParam->addr[0] + height_align*dst_c_stride), dst_c_size*2);
//        }
//    	gettimeofday(&t, NULL);
//        endTime = (int64_t)t.tv_sec*1000000 + t.tv_usec;
//        LOGD("xxxxxx memory copy cost %lld us for %d bytes.", endTime - startTime, dst_y_size + dst_c_size*2);
//    }
#else
    #error "Unknown chip type!"
#endif

#if 0
		{
			static int dec_count = 0;
			static FILE *fp;

			if(dec_count == 60)
			{
				fp = fopen("/data/camera/t.yuv","wb");
				LOGD("write start fp:%d addr:%p",fp,data);
				fwrite(dst,1,buf->stride * buf->height,fp);
				fwrite(dst + buf->stride * buf->height * 5/4,1,buf->stride * buf->height / 4,fp);
				fwrite(dst + buf->stride * buf->height,1,buf->stride * buf->height / 4,fp);
				fclose(fp);
				LOGD("write finish");
			}

			dec_count++;
		}
#endif

    CHECK_EQ(0, mapper.unlock(buf->handle));

    if ((err = mNativeWindow->queueBuffer_DEPRECATED(mNativeWindow.get(), buf)) != 0) {
        LOGW("Surface::queueBuffer returned error %d", err);
    }
    buf = NULL;

}

}  // namespace android
