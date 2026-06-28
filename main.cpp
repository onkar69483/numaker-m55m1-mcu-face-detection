/**************************************************************************//**
 * @file     main.cpp
 * @version  V1.00
 * @brief    Face detection network sample. Demonstrate face detection with bounding box.
 *
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @copyright Copyright (C) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/

#include "BoardInit.hpp"
#include "log_macros.h"

#include "BufAttributes.hpp"
#include "NNModel.hpp"
#include "DetectorPostProcessing.hpp"

#include "imlib.h"
#include "framebuffer.h"

#undef PI /* PI macro conflict with CMSIS/DSP */
#include "NuMicro.h"

//#define __PROFILE__
#define __USE_CCAP__
#define __USE_DISPLAY__
//#define __USE_UVC__

#include "Profiler.hpp"

#if defined(__USE_CCAP__)
    #include "ImageSensor.h"
#endif
#if defined(__USE_DISPLAY__)
    #include "Display.h"
#endif
#if defined(__USE_UVC__)
    #include "UVC.h"
#endif

#define NUM_FRAMEBUF           2
#define FACE_DETECT_THRESHOLD  0.4f

typedef enum
{
    eFRAMEBUF_EMPTY,
    eFRAMEBUF_FULL,
    eFRAMEBUF_INF
} E_FRAMEBUF_STATE;

typedef struct
{
    E_FRAMEBUF_STATE eState;
    image_t frameImage;
    std::vector<arm::app::object_detection::DetectionResult> results;
} S_FRAMEBUF;

S_FRAMEBUF s_asFramebuf[NUM_FRAMEBUF];

namespace arm
{
namespace app
{
/* Tensor arena buffer */
static uint8_t tensorArena[ACTIVATION_BUF_SZ] ACTIVATION_BUF_ATTRIBUTE;

namespace nn
{
extern uint8_t *GetModelPointer();
extern size_t GetModelLen();
} /* namespace nn */
} /* namespace app */
} /* namespace arm */

#define IMAGE_DISP_UPSCALE_FACTOR 1

#if defined(LT7381_LCD_PANEL)
    #define FONT_DISP_UPSCALE_FACTOR 2
#else
    #define FONT_DISP_UPSCALE_FACTOR 1
#endif

#define GLCD_WIDTH  240
#define GLCD_HEIGHT 240

#define IMAGE_FB_SIZE (GLCD_WIDTH * GLCD_HEIGHT * 2)

#undef OMV_FB_SIZE
#define OMV_FB_SIZE (IMAGE_FB_SIZE + 1024)

#undef OMV_FB_ALLOC_SIZE
#define OMV_FB_ALLOC_SIZE (1 * 1024)

__attribute__((section(".bss.sram.data"), aligned(32))) static char fb_array[OMV_FB_SIZE + OMV_FB_ALLOC_SIZE];
__attribute__((section(".bss.sram.data"), aligned(32))) static char jpeg_array[OMV_JPEG_BUF_SIZE];
__attribute__((section(".bss.sram.data"), aligned(32))) static char frame_buf1[OMV_FB_SIZE];

char *_fb_base = NULL;
char *_fb_end  = NULL;
char *_jpeg_buf = NULL;
char *_fballoc = NULL;

static S_FRAMEBUF *get_empty_framebuf()
{
    for (int i = 0; i < NUM_FRAMEBUF; i++)
        if (s_asFramebuf[i].eState == eFRAMEBUF_EMPTY)
            return &s_asFramebuf[i];
    return NULL;
}

static S_FRAMEBUF *get_full_framebuf()
{
    for (int i = 0; i < NUM_FRAMEBUF; i++)
        if (s_asFramebuf[i].eState == eFRAMEBUF_FULL)
            return &s_asFramebuf[i];
    return NULL;
}

static S_FRAMEBUF *get_inf_framebuf()
{
    for (int i = 0; i < NUM_FRAMEBUF; i++)
        if (s_asFramebuf[i].eState == eFRAMEBUF_INF)
            return &s_asFramebuf[i];
    return NULL;
}

static void omv_init()
{
    image_t frameBuffer;

    frameBuffer.w      = GLCD_WIDTH;
    frameBuffer.h      = GLCD_HEIGHT;
    frameBuffer.size   = GLCD_WIDTH * GLCD_HEIGHT * 2;
    frameBuffer.pixfmt = PIXFORMAT_RGB565;

    _fb_base  = fb_array;
    _fb_end   = fb_array + OMV_FB_SIZE - 1;
    _fballoc  = _fb_base + OMV_FB_SIZE + OMV_FB_ALLOC_SIZE;
    _jpeg_buf = jpeg_array;

    fb_alloc_init0();
    framebuffer_init0();
    framebuffer_init_from_image(&frameBuffer);

    for (int i = 0; i < NUM_FRAMEBUF; i++)
        s_asFramebuf[i].eState = eFRAMEBUF_EMPTY;

    framebuffer_init_image(&s_asFramebuf[0].frameImage);

    s_asFramebuf[1].frameImage.w      = GLCD_WIDTH;
    s_asFramebuf[1].frameImage.h      = GLCD_HEIGHT;
    s_asFramebuf[1].frameImage.size   = GLCD_WIDTH * GLCD_HEIGHT * 2;
    s_asFramebuf[1].frameImage.pixfmt = PIXFORMAT_RGB565;
    s_asFramebuf[1].frameImage.data   = (uint8_t *)frame_buf1;
}

static void DrawDetectFace(
    std::vector<arm::app::object_detection::DetectionResult> &results,
    image_t *drawImg)
{
    for (size_t i = 0; i < results.size(); i++)
    {
        arm::app::object_detection::DetectionResult &faceBox = results[i];
        imlib_draw_rectangle(drawImg, faceBox.m_x0, faceBox.m_y0,
                             faceBox.m_w, faceBox.m_h, COLOR_B5_MAX, 2, false);
    }
}

int main()
{
    BoardInit();

    arm::app::NNModel model;

    if (!model.Init(arm::app::tensorArena,
                    sizeof(arm::app::tensorArena),
                    arm::app::nn::GetModelPointer(),
                    arm::app::nn::GetModelLen()))
    {
        printf_err("Failed to initialise model\n");
        return 1;
    }

    info("Set tensor arena cache policy to WTRA\n");
    const std::vector<ARM_MPU_Region_t> mpuConfig =
    {
        {
            ARM_MPU_RBAR(((unsigned int)arm::app::tensorArena),
                         ARM_MPU_SH_NON, 0, 1, 1),
            ARM_MPU_RLAR((((unsigned int)arm::app::tensorArena) + ACTIVATION_BUF_SZ - 1),
                         eMPU_ATTR_CACHEABLE_WTRA)
        },
        {
            ARM_MPU_RBAR(((unsigned int)fb_array),
                         ARM_MPU_SH_NON, 0, 1, 1),
            ARM_MPU_RLAR((((unsigned int)fb_array) + OMV_FB_SIZE - 1),
                         eMPU_ATTR_NON_CACHEABLE)
        },
        {
            ARM_MPU_RBAR(((unsigned int)frame_buf1),
                         ARM_MPU_SH_NON, 0, 1, 1),
            ARM_MPU_RLAR((((unsigned int)frame_buf1) + OMV_FB_SIZE - 1),
                         eMPU_ATTR_NON_CACHEABLE)
        },
    };

    InitPreDefMPURegion(&mpuConfig[0], mpuConfig.size());

    TfLiteTensor *inputTensor = model.GetInputTensor(0);

    if (!inputTensor->dims)
    {
        printf_err("Invalid input tensor dims\n");
        return 2;
    }
    else if (inputTensor->dims->size < 3)
    {
        printf_err("Input tensor dimension should be >= 3\n");
        return 3;
    }

    TfLiteIntArray *inputShape   = model.GetInputShape(0);
    const int inputImgCols       = inputShape->data[arm::app::NNModel::ms_inputColsIdx];
    const int inputImgRows       = inputShape->data[arm::app::NNModel::ms_inputRowsIdx];

    TfLiteTensor *outputTensor0  = model.GetOutputTensor(0);
    TfLiteTensor *outputTensor1  = model.GetOutputTensor(1);

    image_t frameBuffer;
    rectangle_t roi;

    omv_init();
    framebuffer_init_image(&frameBuffer);

    const arm::app::object_detection::PostProcessParams postProcessParams
    {
        inputImgRows,
        inputImgCols,
        (int)s_asFramebuf[0].frameImage.h,
        (int)s_asFramebuf[0].frameImage.w,
        anchor1,
        anchor2,
        FACE_DETECT_THRESHOLD
    };

    arm::app::DetectorPostProcess postProcess(
        outputTensor0, outputTensor1, s_asFramebuf[0].results, postProcessParams);

    pmu_reset_counters();

#define EACH_PERF_SEC 5
    uint64_t u64PerfCycle  = pmu_get_systick_Count() + ((uint64_t)SystemCoreClock * EACH_PERF_SEC);
    uint64_t u64PerfFrames = 0;

    ImageSensor_Init();
    ImageSensor_Config(eIMAGE_FMT_RGB565, GLCD_WIDTH, GLCD_HEIGHT, true);

#if defined(__USE_DISPLAY__)
    char szDisplayText[100];
    S_DISP_RECT sDispRect;

    Display_Init();
    Display_ClearLCD(C_WHITE);
#endif

#if defined(__USE_UVC__)
    UVC_Init();
    HSUSBD_Start();
#endif

    S_FRAMEBUF *emptyFramebuf;
    S_FRAMEBUF *fullFramebuf;
    S_FRAMEBUF *infFramebuf;

    while (1)
    {
        /* --- CAPTURE: kick off DMA into an empty buffer --- */
        emptyFramebuf = get_empty_framebuf();
        if (emptyFramebuf)
            ImageSensor_TriggerCapture((uint32_t)(emptyFramebuf->frameImage.data));

        /* --- INFERENCE: resize + quantize + run model on the last full frame --- */
        fullFramebuf = get_full_framebuf();
        if (fullFramebuf)
        {
            image_t resizeImg;
            roi.x = 0;
            roi.y = 0;
            roi.w = fullFramebuf->frameImage.w;
            roi.h = fullFramebuf->frameImage.h;

            resizeImg.w      = inputImgCols;
            resizeImg.h      = inputImgRows;
            resizeImg.data   = (uint8_t *)inputTensor->data.data;
            resizeImg.pixfmt = PIXFORMAT_GRAYSCALE;

            imlib_nvt_scale(&fullFramebuf->frameImage, &resizeImg, &roi);

            auto *req_data        = static_cast<uint8_t *>(inputTensor->data.data);
            auto *signed_req_data = static_cast<int8_t *>(inputTensor->data.data);
            for (size_t i = 0; i < inputTensor->bytes; i++)
                signed_req_data[i] = static_cast<int8_t>(req_data[i]) - 128;

            model.RunInference();
            fullFramebuf->eState = eFRAMEBUF_INF;
        }

        /* --- DISPLAY: post-process, draw boxes, send to LCD --- */
        infFramebuf = get_inf_framebuf();
        if (infFramebuf)
        {
            postProcess.RunPostProcess(infFramebuf->results);

            if (!infFramebuf->results.empty())
                DrawDetectFace(infFramebuf->results, &infFramebuf->frameImage);

#if defined(__USE_DISPLAY__)
            sDispRect.u32TopLeftX    = 0;
            sDispRect.u32TopLeftY    = 0;
            sDispRect.u32BottonRightX = (infFramebuf->frameImage.w * IMAGE_DISP_UPSCALE_FACTOR) - 1;
            sDispRect.u32BottonRightY = (infFramebuf->frameImage.h * IMAGE_DISP_UPSCALE_FACTOR) - 1;
            Display_FillRect((uint16_t *)infFramebuf->frameImage.data, &sDispRect, IMAGE_DISP_UPSCALE_FACTOR);

            /* Show face detected status — updated every frame */
            const char *faceStatus = infFramebuf->results.empty()
                                     ? "Face Detected: No "
                                     : "Face Detected: Yes";

            sDispRect.u32TopLeftX    = 0;
            sDispRect.u32TopLeftY    = GLCD_HEIGHT * IMAGE_DISP_UPSCALE_FACTOR;
            sDispRect.u32BottonRightX = GLCD_WIDTH * IMAGE_DISP_UPSCALE_FACTOR;
            sDispRect.u32BottonRightY = (GLCD_HEIGHT * IMAGE_DISP_UPSCALE_FACTOR) +
                                        (FONT_HTIGHT * FONT_DISP_UPSCALE_FACTOR) - 1;
            Display_ClearRect(C_WHITE, &sDispRect);
            Display_PutText(faceStatus, strlen(faceStatus),
                            0, GLCD_HEIGHT * IMAGE_DISP_UPSCALE_FACTOR,
                            C_BLUE, C_WHITE, false, FONT_DISP_UPSCALE_FACTOR);
#endif

#if defined(__USE_UVC__)
            if (UVC_IsConnect())
            {
                image_t origImg;
                image_t vflipImg;
                origImg.w = vflipImg.w = infFramebuf->frameImage.w;
                origImg.h = vflipImg.h = infFramebuf->frameImage.h;
                origImg.data = vflipImg.data = (uint8_t *)infFramebuf->frameImage.data;
                origImg.pixfmt = vflipImg.pixfmt = PIXFORMAT_RGB565;
                imlib_nvt_vflip(&origImg, &vflipImg);
                UVC_SendImage((uint32_t)infFramebuf->frameImage.data, IMAGE_FB_SIZE, uvcStatus.StillImage);
            }
#endif

            u64PerfFrames++;
            if ((uint64_t)pmu_get_systick_Count() > u64PerfCycle)
            {
                info("Total inference rate: %llu fps\n", u64PerfFrames / EACH_PERF_SEC);

#if defined(__USE_DISPLAY__)
                sprintf(szDisplayText, "Frame Rate %llu", u64PerfFrames / EACH_PERF_SEC);

                sDispRect.u32TopLeftX    = 0;
                sDispRect.u32TopLeftY    = (GLCD_HEIGHT * IMAGE_DISP_UPSCALE_FACTOR) +
                                           (FONT_HTIGHT * FONT_DISP_UPSCALE_FACTOR);
                sDispRect.u32BottonRightX = GLCD_WIDTH * IMAGE_DISP_UPSCALE_FACTOR;
                sDispRect.u32BottonRightY = (GLCD_HEIGHT * IMAGE_DISP_UPSCALE_FACTOR) +
                                            (2 * FONT_HTIGHT * FONT_DISP_UPSCALE_FACTOR) - 1;

                Display_ClearRect(C_WHITE, &sDispRect);
                Display_PutText(
                    szDisplayText,
                    strlen(szDisplayText),
                    0,
                    (GLCD_HEIGHT * IMAGE_DISP_UPSCALE_FACTOR) + (FONT_HTIGHT * FONT_DISP_UPSCALE_FACTOR),
                    C_BLUE,
                    C_WHITE,
                    false,
                    FONT_DISP_UPSCALE_FACTOR);
#endif
                u64PerfCycle  = (uint64_t)pmu_get_systick_Count() +
                                (uint64_t)SystemCoreClock * EACH_PERF_SEC;
                u64PerfFrames = 0;
            }

            infFramebuf->eState = eFRAMEBUF_EMPTY;
        }

        /* --- Wait for CCAP DMA to finish, mark buffer as ready --- */
        if (emptyFramebuf)
        {
            ImageSensor_WaitCaptureDone();
            emptyFramebuf->eState = eFRAMEBUF_FULL;
        }
    }

    return 0;
}
