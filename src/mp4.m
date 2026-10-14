#include "mp4.h"

#import <AVFoundation/AVFoundation.h>
#include <Accelerate/Accelerate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Frames and sound wait in queues until their writer inputs take them: an
   input may refuse while the encoder is busy, or while the other input is
   behind (the writer interleaves them, in chunks: it can want sound from
   well past the frames it has). The video queue is kept short, since each
   frame is a whole picture, but only while the writer isn't waiting for
   sound: that comes from the caller, after this returns. The sound queue
   isn't limited, so a burst of sound (rendered all at once after a stretch
   with no frames) never has to wait for frames that come after it. */
#define MAX_QUEUED_FRAMES 4
#define MAX_FRAMES_AHEAD 240 /* waiting for sound */
#define WAIT_SECONDS 30

@interface LoonyMP4 : NSObject
@end

@implementation LoonyMP4 {
  @public
    AVAssetWriter *writer;
    AVAssetWriterInput *video, *audio;
    AVAssetWriterInputPixelBufferAdaptor *adaptor;
    CMAudioFormatDescriptionRef audio_format;
    int w, h, fps, rate;
    int in_w, in_h;  /* the frames given */
    uint8_t *scaled; /* a frame scaled to w x h, when in_w x in_h isn't */
    CVPixelBufferRef last; /* the frame shown last */
    NSMutableArray *frames; /* CVPixelBuffers waiting, at the times in frame_times */
    NSMutableArray<NSValue *> *frame_times;
    NSMutableArray *sound; /* CMSampleBuffers waiting */
    bool video_done, audio_done;
    bool at_quality; /* the video is at a quality, not a bitrate */
    NSString *error; /* the first failure */
}

- (void)dealloc {
    if (last)
        CVPixelBufferRelease(last);
    if (audio_format)
        CFRelease(audio_format);
    free(scaled);
}
@end

static void set_err(char *err, size_t errlen, NSString *msg) {
    snprintf(err, errlen, "%s", msg.UTF8String);
}

/* An error's description, with the reason under it when there is one. */
static NSString *describe(NSError *e) {
    if (!e)
        return nil;
    NSError *under = e.userInfo[NSUnderlyingErrorKey];
    NSString *why = e.localizedFailureReason ?: under.localizedDescription;
    return why ? [NSString stringWithFormat:@"%@ (%@)", e.localizedDescription, why]
               : e.localizedDescription;
}

static bool fail(LoonyMP4 *m, NSString *what) {
    if (!m->error) {
        NSError *e = m->writer.error;
        m->error = e ? [NSString stringWithFormat:@"%@: %@", what, describe(e)] : what;
    }
    return false;
}

/* Hands the queued frames and sound to the writer. Waits until at most
   MAX_QUEUED_FRAMES frames are left, or with finish, until everything is
   written: an input whose queue is empty is then marked finished, so the
   writer stops waiting on it to interleave. */
static bool drain(LoonyMP4 *m, bool finish) {
    useconds_t waited_us = 0;
    for (;;) {
        if (m->error)
            return false;
        bool moved = false;
        while (m->sound.count && m->audio.readyForMoreMediaData) {
            if (![m->audio appendSampleBuffer:(__bridge CMSampleBufferRef)m->sound[0]])
                return fail(m, @"can't add sound");
            [m->sound removeObjectAtIndex:0];
            moved = true;
        }
        while (m->frames.count && m->video.readyForMoreMediaData) {
            if (![m->adaptor appendPixelBuffer:(__bridge CVPixelBufferRef)m->frames[0]
                          withPresentationTime:m->frame_times[0].CMTimeValue])
                return fail(m, @"can't add a frame");
            [m->frames removeObjectAtIndex:0];
            [m->frame_times removeObjectAtIndex:0];
            moved = true;
        }
        if (m->writer.status == AVAssetWriterStatusFailed)
            return fail(m, @"the writer failed");
        if (finish) {
            if (!m->sound.count && !m->audio_done) {
                [m->audio markAsFinished];
                m->audio_done = true;
                moved = true;
            }
            if (!m->frames.count && !m->video_done) {
                [m->video markAsFinished];
                m->video_done = true;
                moved = true;
            }
            if (m->audio_done && m->video_done)
                return true;
        } else if (m->frames.count <= MAX_QUEUED_FRAMES ||
                   (!m->sound.count && m->audio.readyForMoreMediaData &&
                    m->frames.count <= MAX_FRAMES_AHEAD)) {
            return true;
        }
        if (moved) {
            waited_us = 0;
            continue;
        }
        if (waited_us >= WAIT_SECONDS * 1000000u)
            return fail(m, @"the encoder stopped taking frames");
        usleep(500);
        waited_us += 500;
    }
}

/* The video's settings: H.264 at s->quality, or with at_quality false, at
   s->video_kbps on average. */
static NSDictionary *video_settings(const mp4_settings *s, bool at_quality) {
    NSMutableDictionary *cp = [@{
        AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
        AVVideoH264EntropyModeKey : AVVideoH264EntropyModeCABAC,
        AVVideoExpectedSourceFrameRateKey : @(s->fps),
        AVVideoMaxKeyFrameIntervalKey : @(s->fps * 4),
        AVVideoAllowFrameReorderingKey : @YES,
    } mutableCopy];
    if (at_quality)
        cp[AVVideoQualityKey] = @(s->quality);
    else
        cp[AVVideoAverageBitRateKey] = @(s->video_kbps * 1000);
    return @{
        AVVideoCodecKey : AVVideoCodecTypeH264,
        AVVideoWidthKey : @(s->w),
        AVVideoHeightKey : @(s->h),
        AVVideoColorPropertiesKey : @{
            AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
            AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
            AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
        },
        AVVideoCompressionPropertiesKey : cp,
    };
}

mp4_file *mp4_open(const char *path, const mp4_settings *s, char *err, size_t errlen) {
    @autoreleasepool {
        LoonyMP4 *m = [[LoonyMP4 alloc] init];
        int w = s->w, h = s->h, rate = s->rate;
        m->w = w;
        m->h = h;
        m->fps = s->fps;
        m->rate = rate;
        m->in_w = s->in_w ? s->in_w : w;
        m->in_h = s->in_h ? s->in_h : h;
        if ((m->in_w != w || m->in_h != h) && !(m->scaled = malloc((size_t)w * (size_t)h * 4))) {
            snprintf(err, errlen, "out of memory");
            return NULL;
        }
        m->frames = [NSMutableArray array];
        m->frame_times = [NSMutableArray array];
        m->sound = [NSMutableArray array];
        NSURL *url = [NSURL fileURLWithPath:@(path)];
        NSString *ext = url.pathExtension.lowercaseString;
        unlink(path); /* AVAssetWriter won't write over a file */
        NSError *e = nil;
        m->writer = [AVAssetWriter assetWriterWithURL:url
                                             fileType:[ext isEqualToString:@"mov"] ? AVFileTypeQuickTimeMovie
                                                                                   : AVFileTypeMPEG4
                                                error:&e];
        if (!m->writer) {
            set_err(err, errlen, describe(e));
            return NULL;
        }
        m->writer.shouldOptimizeForNetworkUse = YES; /* the index first, so it plays as it downloads */

        /* An encoder without a quality setting (Apple's software H.264
           encoder) makes the input throw. */
        m->at_quality = s->quality > 0;
        if (m->at_quality) {
            @try {
                m->video = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                              outputSettings:video_settings(s, true)];
            } @catch (NSException *x) {
                (void)x;
                m->at_quality = false;
            }
        }
        if (!m->video)
            m->video = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                          outputSettings:video_settings(s, false)];
        m->video.expectsMediaDataInRealTime = NO;
        m->adaptor = [AVAssetWriterInputPixelBufferAdaptor
            assetWriterInputPixelBufferAdaptorWithAssetWriterInput:m->video
                                       sourcePixelBufferAttributes:@{
                                           (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                                           (id)kCVPixelBufferWidthKey : @(w),
                                           (id)kCVPixelBufferHeightKey : @(h),
                                       }];

        AudioStreamBasicDescription pcm = {
            .mSampleRate = rate,
            .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
            .mBytesPerPacket = 4,
            .mFramesPerPacket = 1,
            .mBytesPerFrame = 4,
            .mChannelsPerFrame = 2,
            .mBitsPerChannel = 16,
        };
        if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &pcm, 0, NULL, 0, NULL, NULL,
                                           &m->audio_format) != noErr) {
            snprintf(err, errlen, "can't describe the sound");
            return NULL;
        }
        AudioChannelLayout stereo = {.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo};
        NSDictionary *as = @{
            AVFormatIDKey : @(kAudioFormatMPEG4AAC),
            AVSampleRateKey : @(rate),
            AVNumberOfChannelsKey : @2,
            AVChannelLayoutKey : [NSData dataWithBytes:&stereo length:sizeof stereo],
            AVEncoderBitRateKey : @(s->audio_kbps * 1000),
        };
        m->audio = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                      outputSettings:as
                                                    sourceFormatHint:m->audio_format];
        m->audio.expectsMediaDataInRealTime = NO;

        if (![m->writer canAddInput:m->video] || ![m->writer canAddInput:m->audio]) {
            snprintf(err, errlen, "can't write %dx%d H.264 with AAC sound to %s", w, h, path);
            return NULL;
        }
        [m->writer addInput:m->video];
        [m->writer addInput:m->audio];
        if (![m->writer startWriting]) {
            set_err(err, errlen, describe(m->writer.error) ?: @"can't start writing");
            return NULL;
        }
        [m->writer startSessionAtSourceTime:kCMTimeZero];
        return (__bridge_retained mp4_file *)m;
    }
}

bool mp4_at_quality(const mp4_file *f) { return ((__bridge LoonyMP4 *)f)->at_quality; }

/* A new pixel buffer holding rgba (in_w x in_h), as BGRA, scaled to w x h. */
static CVPixelBufferRef make_frame(LoonyMP4 *m, const uint8_t *rgba) {
    CVPixelBufferRef pb = NULL;
    CVPixelBufferPoolRef pool = m->adaptor.pixelBufferPool;
    if (pool ? CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb) != kCVReturnSuccess
             : CVPixelBufferCreate(kCFAllocatorDefault, (size_t)m->w, (size_t)m->h,
                                   kCVPixelFormatType_32BGRA, NULL, &pb) != kCVReturnSuccess)
        return NULL;
    CVPixelBufferLockBaseAddress(pb, 0);
    vImage_Buffer src = {(void *)rgba, (vImagePixelCount)m->in_h, (vImagePixelCount)m->in_w,
                         (size_t)m->in_w * 4};
    vImage_Buffer dst = {CVPixelBufferGetBaseAddress(pb), (vImagePixelCount)m->h,
                         (vImagePixelCount)m->w, CVPixelBufferGetBytesPerRow(pb)};
    const uint8_t to_bgra[4] = {2, 1, 0, 3};
    vImage_Error e = kvImageNoError;
    if (m->scaled) { /* the channels' order doesn't matter to the scaling */
        vImage_Buffer mid = {m->scaled, (vImagePixelCount)m->h, (vImagePixelCount)m->w, (size_t)m->w * 4};
        e = vImageScale_ARGB8888(&src, &mid, NULL, kvImageHighQualityResampling);
        src = mid;
    }
    if (e == kvImageNoError)
        e = vImagePermuteChannels_ARGB8888(&src, &dst, to_bgra, kvImageNoFlags);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    if (e != kvImageNoError) {
        CVPixelBufferRelease(pb);
        return NULL;
    }
    return pb;
}

bool mp4_video(mp4_file *f, const uint8_t *rgba, int64_t frame) {
    LoonyMP4 *m = (__bridge LoonyMP4 *)f;
    @autoreleasepool {
        if (m->error)
            return false;
        if (rgba) {
            CVPixelBufferRef pb = make_frame(m, rgba);
            if (!pb)
                return fail(m, @"can't make a frame");
            if (m->last)
                CVPixelBufferRelease(m->last);
            m->last = pb;
        } else if (!m->last) {
            return fail(m, @"no frame to show again");
        }
        [m->frames addObject:(__bridge id)m->last];
        [m->frame_times addObject:[NSValue valueWithCMTime:CMTimeMake(frame, m->fps)]];
        return drain(m, false);
    }
}

bool mp4_audio(mp4_file *f, const int16_t *samples, uint32_t frames, int64_t at) {
    LoonyMP4 *m = (__bridge LoonyMP4 *)f;
    @autoreleasepool {
        if (m->error)
            return false;
        if (frames == 0)
            return true;
        size_t bytes = (size_t)frames * 4;
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sb = NULL;
        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, bytes, kCFAllocatorDefault,
                                               NULL, 0, bytes, kCMBlockBufferAssureMemoryNowFlag,
                                               &block) != noErr)
            return fail(m, @"out of memory for sound");
        OSStatus s = CMBlockBufferReplaceDataBytes(samples, block, 0, bytes);
        if (s == noErr)
            s = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
                kCFAllocatorDefault, block, m->audio_format, (CMItemCount)frames,
                CMTimeMake(at, m->rate), NULL, &sb);
        CFRelease(block);
        if (s != noErr)
            return fail(m, @"can't wrap the sound");
        [m->sound addObject:(__bridge_transfer id)sb];
        return drain(m, false);
    }
}

bool mp4_close(mp4_file *f, char *err, size_t errlen) {
    LoonyMP4 *m = (__bridge_transfer LoonyMP4 *)f;
    @autoreleasepool {
        if (drain(m, true)) {
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [m->writer finishWritingWithCompletionHandler:^{
              dispatch_semaphore_signal(done);
            }];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            if (m->writer.status != AVAssetWriterStatusCompleted)
                fail(m, @"can't finish the file");
        } else {
            [m->writer cancelWriting];
        }
        if (m->error)
            set_err(err, errlen, m->error);
        return !m->error;
    }
}

bool mp4_read_info(const char *path, mp4_info *out, char *err, size_t errlen) {
    @autoreleasepool {
        memset(out, 0, sizeof *out);
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(path)] options:nil];
        NSError *e = nil;
        AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:&e];
        if (!reader) {
            set_err(err, errlen, describe(e) ?: @"can't read the file");
            return false;
        }
        /* Synchronous track access is deprecated, but the tests want it plain. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        AVAssetTrack *vt = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
        AVAssetTrack *at = [asset tracksWithMediaType:AVMediaTypeAudio].firstObject;
        if (!vt) {
            snprintf(err, errlen, "%s has no video", path);
            return false;
        }
        out->width = (int)vt.naturalSize.width;
        out->height = (int)vt.naturalSize.height;
        out->video_seconds = CMTimeGetSeconds(vt.timeRange.duration);
        if (at)
            out->audio_seconds = CMTimeGetSeconds(at.timeRange.duration);
#pragma clang diagnostic pop
        /* Frames counted from the samples, without decoding them; the sound decoded to find its peak. */
        AVAssetReaderTrackOutput *vo = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:vt
                                                                                  outputSettings:nil];
        [reader addOutput:vo];
        AVAssetReaderTrackOutput *ao = nil;
        if (at) {
            ao = [AVAssetReaderTrackOutput
                assetReaderTrackOutputWithTrack:at
                                 outputSettings:@{
                                     AVFormatIDKey : @(kAudioFormatLinearPCM),
                                     AVLinearPCMBitDepthKey : @16,
                                     AVLinearPCMIsFloatKey : @NO,
                                     AVLinearPCMIsBigEndianKey : @NO,
                                     AVLinearPCMIsNonInterleaved : @NO,
                                 }];
            [reader addOutput:ao];
        }
        if (![reader startReading]) {
            set_err(err, errlen, describe(reader.error) ?: @"can't read the file");
            return false;
        }
        CMSampleBufferRef sb;
        while ((sb = [vo copyNextSampleBuffer])) {
            out->video_frames += CMSampleBufferGetNumSamples(sb);
            CFRelease(sb);
        }
        int peak = 0;
        while (ao && (sb = [ao copyNextSampleBuffer])) {
            CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sb);
            size_t len = 0;
            char *data = NULL;
            if (block && CMBlockBufferGetDataPointer(block, 0, NULL, &len, &data) == noErr)
                for (size_t i = 0; i + 1 < len; i += 2) {
                    int v = abs((int)*(const int16_t *)(const void *)(data + i));
                    if (v > peak)
                        peak = v;
                }
            CFRelease(sb);
        }
        out->peak = peak / 32768.0;
        if (reader.status == AVAssetReaderStatusFailed) {
            set_err(err, errlen, describe(reader.error) ?: @"can't read the file");
            return false;
        }
        return true;
    }
}
