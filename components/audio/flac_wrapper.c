#include "flac_wrapper.h"

#include "codecs/FLAC/format.h"
#include "codecs/FLAC/stream_decoder.h"

#include <memory.h>
#include <esp_log.h>

#define I2S_NUM 0

static const char TAG[] = "audio_flac";

static FLAC__StreamDecoder* decoder_ptr;

static FLAC__StreamDecoderReadStatus read_callback(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void* ctx)
{
    AudioContext_t* audio_ctx = ctx;

    size_t read = audio_ctx->fill_buffer(buffer, *bytes);
    if (read == 0)
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    else if (read < *bytes) {
        *bytes = read;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void* ctx) {
    AudioContext_t* audio_ctx = ctx;

    unsigned short right_i = frame->header.channels == 2 ? 1 : 0;

    bool ret = audio_ctx->write(buffer[0], buffer[right_i], frame->header.blocksize, frame->header.sample_rate, frame->header.bits_per_sample);
    if (ret == false)
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void* ctx) {
    ESP_LOGE(TAG, "Decoder failed: %s", FLAC__StreamDecoderErrorStatusString[status]);

    AudioContext_t* audio_ctx = ctx;
    audio_ctx->decoder_failed();
}

static FLAC__StreamDecoderSeekStatus seek_callback(const FLAC__StreamDecoder *decoder, FLAC__uint64 absolute_byte_offset, void* ctx) {
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus tell_callback(const FLAC__StreamDecoder *decoder, FLAC__uint64 *absolute_byte_offset, void* ctx) {
    AudioContext_t* audio_ctx = ctx;
    *absolute_byte_offset = audio_ctx->bytes_elapsed();
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus length_callback(const FLAC__StreamDecoder *decoder, FLAC__uint64 *stream_length, void* ctx) {
    AudioContext_t* audio_ctx = ctx;
    *stream_length = audio_ctx->total_bytes();

    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool eof_callback(const FLAC__StreamDecoder *decoder, void* ctx) {
    AudioContext_t* audio_ctx = ctx;
    return audio_ctx->eof();
}

void delete_flac_decoder(void) {
    FLAC__stream_decoder_delete(decoder_ptr);
}

void run_flac_decoder(const AudioContext_t* audio_ctx) {
    FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_stream(decoder_ptr,
                                                                            read_callback, seek_callback, tell_callback,
                                                                            length_callback, eof_callback,
                                                                            write_callback, NULL,
                                                                            error_callback, audio_ctx);
    assert(status == FLAC__STREAM_DECODER_INIT_STATUS_OK);

    FLAC__bool b = FLAC__stream_decoder_process_until_end_of_stream(decoder_ptr);
    if (b == true)
        audio_ctx->decoder_finished();

    b = FLAC__stream_decoder_finish(decoder_ptr);
    assert(b);
}

void init_flac_decoder(void) {
    decoder_ptr = FLAC__stream_decoder_new();
    assert (decoder_ptr != NULL);
}

const DecoderWrapper_t flac_wrapper = {
        .init = init_flac_decoder,
        .run = run_flac_decoder,
        .delete = delete_flac_decoder
};