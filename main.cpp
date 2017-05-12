#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h> // V4L
#include <sys/mman.h>   // mmap
#include <pthread.h>

#include "jpegdec.h"

// The headers are not aware C++ exists
extern "C"
{
#include "codec.h"
}

#include "ion.h"
#include "meson_ion.h"

const size_t EXTERNAL_PTS = 0x01;
const size_t SYNC_OUTSIDE = 0x02;
const size_t USE_IDR_FRAMERATE = 0x04;
const size_t UCODE_IP_ONLY_PARAM = 0x08;
const size_t MAX_REFER_BUF = 0x10;
const size_t ERROR_RECOVERY_MODE_IN = 0x20;

static const int BUFFER_COUNT = 8;

static unsigned long startTimeInUs = 0;

double getMsFromStart() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned long timeInUs = 1000000 * tv.tv_sec + tv.tv_usec - startTimeInUs;
    return timeInUs / 1000.0;
}

static void set_freerun_enabled(bool value)
{
    int fd = open("/dev/amvideo", O_RDWR | O_TRUNC);
    if (fd < 0) {
        printf("could not open /dev/amvideo (%x)\n", fd);
    } else {
        int ret = ioctl(fd, AMSTREAM_IOC_SET_FREERUN_MODE, value ? 1 : 0);
        if (ret < 0)
            printf("AMSTREAM_IOC_SET_FREERUN_MODE failed (%x)\n", ret);

        int mode = 0;
        ret = ioctl(fd, AMSTREAM_IOC_GET_FREERUN_MODE, &mode);

        printf("AMSTREAM_IOC_GET_FREERUN_MODE (%x)\n", mode);
        close(fd);
    }
}

codec_para_t codecContext;
void OpenCodec(int width, int height, int fps)
{
    set_freerun_enabled(true);

    // Initialize the codec
    memset(&codecContext, 0, sizeof(codecContext));

    codecContext.stream_type = STREAM_TYPE_ES_VIDEO;
    codecContext.video_type = VFORMAT_MJPEG;
    codecContext.has_video = 1;
    codecContext.noblock = 0;
    codecContext.am_sysinfo.format = VIDEO_DEC_FORMAT_MJPEG;
    codecContext.am_sysinfo.width = width;
    codecContext.am_sysinfo.height = height;
    codecContext.am_sysinfo.rate = 96000 / fps;
    codecContext.am_sysinfo.param = (void *)(EXTERNAL_PTS | SYNC_OUTSIDE); //EXTERNAL_PTS |

    int api = codec_init(&codecContext);
    if (api != 0)
        fprintf(stderr, "Error initializing codec\n");;
}

void WriteCodecData(unsigned char *data, int dataLength)
{
    int offset = 0;
    while (offset < dataLength)
    {
        int count = codec_write(&codecContext, data + offset, dataLength - offset);
        if (count > 0)
            offset += count;
    }
}

struct IonBuffer
{
    ion_user_handle_t Handle;
    int ExportHandle;
    size_t Length;
    unsigned long PhysicalAddress;
};

IonBuffer IonAllocate(int ion_fd, size_t bufferSize)
{
    int io;
    IonBuffer result;

    // Allocate a buffer
    ion_allocation_data allocation_data;
    memset(&allocation_data, 0, sizeof(allocation_data));
    allocation_data.len = bufferSize;
    allocation_data.heap_id_mask = ION_HEAP_CARVEOUT_MASK;
    allocation_data.flags = ION_FLAG_CACHED;

    io = ioctl(ion_fd, ION_IOC_ALLOC, &allocation_data);
    if (io != 0)
        fprintf(stderr, "Error ION_IOC_ALLOC\n");

    printf("ion handle=%d\n", allocation_data.handle);

    // Map/share the buffer
    ion_fd_data ionData = { };
    ionData.handle = allocation_data.handle;

    io = ioctl(ion_fd, ION_IOC_SHARE, &ionData);
    if (io != 0)
        fprintf(stderr, "Error ION_IOC_SHARE\n");
    printf("ion map=%d\n", ionData.fd);

    // Get the physical address for the buffer
    meson_phys_data physData = { };
    physData.handle = ionData.fd;

    ion_custom_data ionCustomData = { };
    ionCustomData.cmd = ION_IOC_MESON_PHYS_ADDR;
    ionCustomData.arg = (long unsigned int)&physData;

    io = ioctl(ion_fd, ION_IOC_CUSTOM, &ionCustomData);
    if (io != 0)
        fprintf(stderr, "Error ION_IOC_CUSTOM\n");

    result.Handle = allocation_data.handle;
    result.ExportHandle = ionData.fd;
    result.Length = allocation_data.len;
    result.PhysicalAddress = physData.phys_addr;

    printf("ion phys_addr=%lu\n", result.PhysicalAddress);

    return result;
}

enum class PictureFormat
{
    Unknown = 0,
    Yuyv = V4L2_PIX_FMT_YUYV,
    MJpeg = V4L2_PIX_FMT_MJPEG
};

void WriteToFile(const char *path, const char *value)
{
    int fd = open(path, O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        printf("WriteToFile open failed: %s = %s\n", path, value);
        exit(1);
    }

    if (write(fd, value, strlen(value)) < 0) {
        printf("WriteToFile write failed: %s = %s\n", path, value);
        exit(1);
    }

    close(fd);
}

void SetVfmState()
{
    WriteToFile("/sys/class/vfm/map", "rm default");
    WriteToFile("/sys/class/vfm/map", "add default decoder ionvideo");
}

void ResetVfmState()
{
    WriteToFile("/sys/class/vfm/map", "rm default");
    WriteToFile("/sys/class/vfm/map", "add default decoder ppmgr deinterlace amvideo");
}

struct IonInfo
{
    int IonFD;
    int IonVideoFD;
    size_t BufferSize;
    unsigned long PhysicalAddress;
    int VideoBufferDmaBufferFD[BUFFER_COUNT];
};

IonInfo OpenIonVideoCapture(int width, int height)
{
    const int VIDEO_WIDTH = width;
    const int VIDEO_HEIGHT = height;
    const unsigned int VIDEO_FORMAT = V4L2_PIX_FMT_NV12;
    const int VIDEO_FRAME_SIZE = VIDEO_WIDTH * VIDEO_HEIGHT;

    IonInfo info;
    memset(&info, 0, sizeof(info));

    info.IonVideoFD = open("/dev/video13", O_RDWR | O_NONBLOCK); //| O_NONBLOCK
    if (info.IonVideoFD < 0)
        fprintf(stderr, "Error opening video13\n");

    info.IonFD = open("/dev/ion", O_RDWR);
    if (info.IonFD < 0)
        fprintf(stderr, "Error opening /dev/ion\n");

    // Set the capture format
    v4l2_format format;
    memset(&format, 0, sizeof(format));

    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = VIDEO_WIDTH;
    format.fmt.pix.height = VIDEO_HEIGHT;
    format.fmt.pix.width = VIDEO_WIDTH;
    format.fmt.pix.height = VIDEO_HEIGHT;
    format.fmt.pix.pixelformat = VIDEO_FORMAT;

    int v4lcall = ioctl(info.IonVideoFD, VIDIOC_S_FMT, &format);
    if (v4lcall < 0)
        fprintf(stderr, "Error VIDIOC_S_FMT\n");


    // Request buffers

    v4l2_requestbuffers requestBuffers;
    memset(&requestBuffers, 0, sizeof(requestBuffers));
    requestBuffers.count = BUFFER_COUNT;
    requestBuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    requestBuffers.memory = V4L2_MEMORY_DMABUF;

    v4lcall = ioctl(info.IonVideoFD, VIDIOC_REQBUFS, &requestBuffers);
    if (v4lcall < 0)
        fprintf(stderr, "ERROR VIDIOC_REQBUFS\n");

    // Allocate buffers
    int videoFrameLength = 0;
    switch (format.fmt.pix_mp.pixelformat) {
    case V4L2_PIX_FMT_RGB32:
        videoFrameLength = VIDEO_FRAME_SIZE * 4;
        break;
    case V4L2_PIX_FMT_RGB24:
        videoFrameLength = VIDEO_FRAME_SIZE * 3;
        break;
    case V4L2_PIX_FMT_NV12:
        videoFrameLength = VIDEO_FRAME_SIZE * 2;
        break;

    default:
        fprintf(stderr, "ERROR wrong pixel format\n");
    }

    info.BufferSize = videoFrameLength;

    for (int i = 0; i < BUFFER_COUNT; ++i) {
        // Allocate a buffer
        ion_allocation_data allocation_data = {  };
        allocation_data.len = videoFrameLength;
        allocation_data.heap_id_mask = ION_HEAP_CARVEOUT_MASK;
        allocation_data.flags = ION_FLAG_CACHED;

        int ionCall = ioctl(info.IonFD, ION_IOC_ALLOC, &allocation_data);
        if (ionCall < 0)
            fprintf(stderr, "ERROR ION_IOC_ALLOC\n");

        // Export the dma_buf
        ion_fd_data fd_data = {  };
        fd_data.handle = allocation_data.handle;

        ionCall = ioctl(info.IonFD, ION_IOC_SHARE, &fd_data);
        if (ionCall < 0)
            fprintf(stderr, "ERROR ION_IOC_SHARE\n");

        info.VideoBufferDmaBufferFD[i] = fd_data.fd;

        // Physical Address
        // Get the physical address for the buffer
        meson_phys_data physData;
        memset(&physData, 0, sizeof(physData));
        physData.handle = fd_data.fd;

        ion_custom_data ionCustomData;
        memset(&ionCustomData, 0, sizeof(ionCustomData));
        ionCustomData.cmd = ION_IOC_MESON_PHYS_ADDR;
        ionCustomData.arg = (long unsigned int)&physData;

        ionCall = ioctl(info.IonFD, ION_IOC_CUSTOM, &ionCustomData);
        if (ionCall < 0)
            fprintf(stderr, "ERROR ION_IOC_CUSTOM\n");

        info.PhysicalAddress = physData.phys_addr;

        // Queue the buffer for V4L to use
        v4l2_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));

        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_DMABUF;
        buffer.index = i;
        buffer.m.fd = info.VideoBufferDmaBufferFD[i];
        buffer.length = 1;  // Ionvideo only supports single plane

        v4lcall = ioctl(info.IonVideoFD, VIDIOC_QBUF, &buffer);
        if (v4lcall < 0)
            fprintf(stderr, "ERROR VIDIOC_BUF\n");

        // DEBUG
        printf("Queued v4l2_buffer:\n");
        printf("\tindex=%x\n", buffer.index);
        printf("\ttype=%x\n", buffer.type);
        printf("\tm.fd=%x\n", buffer.m.fd);
    }

    // Start "streaming"
    int bufferType = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    v4lcall = ioctl(info.IonVideoFD, VIDIOC_STREAMON, &bufferType);
    if (v4lcall < 0)
        fprintf(stderr, "ERROR VIDIOC_STREAMON\n");

    return info;
}

static int ion_fd = -1;
static IonBuffer YuvSource = {0, 0, 0, 0};
static IonInfo ionInfo;
//static void* yuvSourcePtr = nullptr;
static void *jpegData;
static int jpegDataLength = 0;

void *uploadData(void *) {
    static int frameNum = 0;
    double time;
    while (true) {
        time = getMsFromStart();
        WriteCodecData(reinterpret_cast<unsigned char *>(jpegData), jpegDataLength);
        printf("frame %i uploaded in %f ms\n", frameNum, getMsFromStart());
        frameNum++;
        usleep(16666 - (getMsFromStart() - time) * 1000);// sleep for 16.666 ms (which time of 1 frame in 60 fps frame rate)
    }
    return NULL;
}

void checkForFrame()
{
    static int frameNum = 0;
    v4l2_buffer ionBuffer;
    memset(&ionBuffer, 0, sizeof(ionBuffer));
    ionBuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ionBuffer.memory = V4L2_MEMORY_DMABUF;

    int io;

    while (true) {
        io = ioctl(ionInfo.IonVideoFD, VIDIOC_DQBUF, &ionBuffer);
        if (io != 0)
            break;
        printf("Frame %i received in %f ms\n", frameNum++, getMsFromStart());
        ioctl(ionInfo.IonVideoFD, VIDIOC_QBUF, &ionBuffer);
    };
}

int main(int /*argc*/, char **/*argv*/)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    startTimeInUs = 1000000 * tv.tv_sec + tv.tv_usec;
    int width = 1280;
    int height = 720;

    ion_fd = open("/dev/ion", O_RDWR);
    if (ion_fd <= 0)
        fprintf(stderr, "ERROR opening /dev/ion\n");

    YuvSource = IonAllocate(ion_fd, width * height * 4);

    // Ionvideo
    SetVfmState();

    ionInfo = OpenIonVideoCapture(width, height);

    // Create MJPEG codec
    OpenCodec(width, height, 60 /*fps*/);

    auto jpegFile = fopen("frame.jpg", "r");

    if (!jpegFile) {
        fprintf(stderr, "ERROR no frame.jpg file\n");
        return 1;
    }

    fseek (jpegFile, 0, SEEK_END);
    jpegDataLength = ftell(jpegFile);
    fseek (jpegFile, 0, SEEK_SET);
    jpegData = malloc(jpegDataLength);
    if (jpegData) {
        fread(jpegData, 1, jpegDataLength, jpegFile);

    } else {
        fprintf(stderr, "Can't read jpeg file\n");
        return -1;
    }
    fclose(jpegFile);

    pthread_t uploadDataThread;

    /* create a second thread which executes inc_x(&x) */
    if(pthread_create(&uploadDataThread, (pthread_attr_t *)NULL, uploadData, (void *)NULL)) {
        fprintf(stderr, "Error creating thread\n");
        return 1;

    }

    while (true) {
        usleep(1000); // check every 1 ms
        checkForFrame();
    }


//    yuvSourcePtr = mmap(NULL,
//                        ionInfo.BufferSize,
//                        PROT_READ | PROT_WRITE,
//                        MAP_FILE | MAP_SHARED,
//                        ionInfo.VideoBufferDmaBufferFD[ionBuffer.index],
//            0);
//    Q_ASSERT(yuvSourcePtr);

//    QFile fout("/root/frame.rgb");
//    fout.open(QIODevice::WriteOnly);
//    fout.write(reinterpret_cast<const char *>(yuvSourcePtr), ionInfo.BufferSize);
//    fout.close();
//    munmap(yuvSourcePtr, ionInfo.BufferSize);

    return 0;
}
