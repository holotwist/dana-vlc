#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <vlc_codec.h>
#include <vlc_meta.h>
#include <vlc_input.h>

#include "DANADecoder.h"
#include "DANAInternal.h"

#define DANA_FOURCC VLC_FOURCC('D','A','N','A')

// Helper conversions
static inline vlc_tick_t dana_tick_from_samples(uint64_t samples, uint32_t srate) {
    return (srate > 0) ? (vlc_tick_t)(((uint64_t)samples * CLOCK_FREQ) / srate) : 0;
}

static inline uint64_t dana_samples_from_tick(vlc_tick_t tick, uint32_t srate) {
    return (uint64_t)(((uint64_t)tick * srate) / CLOCK_FREQ);
}

// Module descriptor
static int  OpenDemux(vlc_object_t *);
static void CloseDemux(vlc_object_t *);
static int  OpenDecoder(vlc_object_t *);
static void CloseDecoder(vlc_object_t *);

vlc_module_begin()
    set_shortname("Dana")
    set_description("Dana Audio Demuxer and Decoder")
    set_capability("demux", 140)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_DEMUX)
    set_callbacks(OpenDemux, CloseDemux)

    add_submodule()
    set_shortname("Dana Decoder")
    set_description("Dana Audio Decoder")
    set_capability("audio decoder", 100)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACODEC)
    set_callbacks(OpenDecoder, CloseDecoder)
vlc_module_end()

// Demuxer implementation

struct demux_sys_t {
    es_out_id_t *p_es;
    struct DANAHeaderInfo header;
    uint32_t header_size;
    uint64_t current_sample;
    bool b_eof;
    input_attachment_t *p_art;
};

static int Demux(demux_t *p_demux);
static int Control(demux_t *p_demux, int i_query, va_list args);

static int OpenDemux(vlc_object_t *p_this) {
    demux_t *p_demux = (demux_t *)p_this;
    const uint8_t *p_peek;

    // Check magic signature 'D', 'A', 0xFF, 0x00
    if (vlc_stream_Peek(p_demux->s, &p_peek, 4) < 4) return VLC_EGENERIC;
    if (p_peek[0] != 'D' || p_peek[1] != 'A' || p_peek[2] != 0xFF || p_peek[3] != 0x00) {
        return VLC_EGENERIC;
    }

    struct demux_sys_t *p_sys = vlc_obj_malloc(p_this, sizeof(struct demux_sys_t));
    if (!p_sys) return VLC_ENOMEM;
    memset(p_sys, 0, sizeof(struct demux_sys_t));

    // Read header size offset
    if (vlc_stream_Peek(p_demux->s, &p_peek, 8) < 8) return VLC_EGENERIC;
    uint32_t offset = (((uint32_t)p_peek[4] << 24) |
                       ((uint32_t)p_peek[5] << 16) |
                       ((uint32_t)p_peek[6] << 8)  |
                       p_peek[7]);
    p_sys->header_size = offset + 8;

    uint8_t *p_header_buf = malloc(p_sys->header_size);
    if (!p_header_buf) return VLC_ENOMEM;

    if (vlc_stream_Read(p_demux->s, p_header_buf, p_sys->header_size) < (ssize_t)p_sys->header_size) {
        free(p_header_buf);
        return VLC_EGENERIC;
    }

    uint32_t parsed_hdr_sz = 0;
    if (DANADecoder_DecodeHeader(p_header_buf, p_sys->header_size, &p_sys->header, &parsed_hdr_sz) != DANA_APIRESULT_OK) {
        free(p_header_buf);
        return VLC_EGENERIC;
    }

    // Register Elementary Stream (ES)
    es_format_t fmt;
    es_format_Init(&fmt, AUDIO_ES, DANA_FOURCC);
    fmt.audio.i_channels = p_sys->header.wave_format.num_channels;
    fmt.audio.i_rate = p_sys->header.wave_format.sampling_rate;
    fmt.audio.i_bitspersample = p_sys->header.wave_format.bit_per_sample;
    fmt.audio.i_physical_channels = (fmt.audio.i_channels == 1) ? AOUT_CHAN_CENTER : AOUT_CHANS_STEREO;
    fmt.i_bitrate = p_sys->header.max_bit_per_second;

    // Pass serialized header as extradata to decoder
    fmt.i_extra = p_sys->header_size;
    fmt.p_extra = p_header_buf;

    p_sys->p_es = es_out_Add(p_demux->out, &fmt);
    es_format_Clean(&fmt);

    // Populate VLC Metadata
    vlc_meta_t *p_meta = vlc_meta_New();
    if (p_meta) {
        if (p_sys->header.metadata.title)  vlc_meta_SetTitle(p_meta, p_sys->header.metadata.title);
        if (p_sys->header.metadata.artist) vlc_meta_SetArtist(p_meta, p_sys->header.metadata.artist);
        if (p_sys->header.metadata.album)  vlc_meta_SetAlbum(p_meta, p_sys->header.metadata.album);
        if (p_sys->header.metadata.genre)  vlc_meta_SetGenre(p_meta, p_sys->header.metadata.genre);
        if (p_sys->header.metadata.track)  vlc_meta_SetTrackNum(p_meta, p_sys->header.metadata.track);
        if (p_sys->header.metadata.year)   vlc_meta_SetDate(p_meta, p_sys->header.metadata.year);

        if (p_sys->header.metadata.cover_data && p_sys->header.metadata.cover_size > 0) {
            vlc_meta_SetArtURL(p_meta, "attachment://art.jpg");
            p_sys->p_art = vlc_input_attachment_New(
                "art.jpg",
                (p_sys->header.metadata.cover_size >= 8 && memcmp(p_sys->header.metadata.cover_data, "\x89PNG\r\n\x1a\n", 8) == 0)
                    ? "image/png" : "image/jpeg",
                NULL,
                p_sys->header.metadata.cover_data,
                p_sys->header.metadata.cover_size
            );
        }
        es_out_Control(p_demux->out, ES_OUT_SET_GROUP_META, 0, p_meta);
        vlc_meta_Delete(p_meta);
    }

    p_demux->p_sys = p_sys;
    p_demux->pf_demux = Demux;
    p_demux->pf_control = Control;

    return VLC_SUCCESS;
}

static void CloseDemux(vlc_object_t *p_this) {
    demux_t *p_demux = (demux_t *)p_this;
    struct demux_sys_t *p_sys = p_demux->p_sys;
    if (p_sys->p_art) vlc_input_attachment_Delete(p_sys->p_art);
    DANAMetadata_Release(&p_sys->header.metadata);
}

static int Demux(demux_t *p_demux) {
    struct demux_sys_t *p_sys = p_demux->p_sys;
    if (p_sys->b_eof) return VLC_DEMUXER_EOF;

    const uint8_t *p_peek;
    if (vlc_stream_Peek(p_demux->s, &p_peek, 5) < 5) {
        p_sys->b_eof = true;
        return VLC_DEMUXER_EOF;
    }

    uint16_t sync_code = ((uint16_t)p_peek[0] << 8) | p_peek[1];
    if (sync_code != DANA_BLOCK_SYNC_CODE) {
        p_sys->b_eof = true;
        return VLC_DEMUXER_EOF;
    }

    uint32_t block_size = (((uint32_t)p_peek[2] << 16) |
                           ((uint32_t)p_peek[3] << 8)  |
                           p_peek[4]) + 5;

    block_t *p_block = vlc_stream_Block(p_demux->s, block_size);
    if (!p_block) {
        p_sys->b_eof = true;
        return VLC_DEMUXER_EOF;
    }

    vlc_tick_t pts = dana_tick_from_samples(p_sys->current_sample, p_sys->header.wave_format.sampling_rate);
    p_block->i_dts = p_block->i_pts = VLC_TICK_0 + pts;

    // Extract num_samples from block header
    uint32_t num_samples = 0;
    if (p_block->i_buffer >= 9) {
        num_samples = ((uint32_t)p_block->p_buffer[7] << 8) | p_block->p_buffer[8];
    }

    p_block->i_length = dana_tick_from_samples(num_samples, p_sys->header.wave_format.sampling_rate);
    p_sys->current_sample += num_samples;

    es_out_SetPCR(p_demux->out, p_block->i_pts);
    es_out_Send(p_demux->out, p_sys->p_es, p_block);

    return VLC_DEMUXER_SUCCESS;
}

static int FindNearestBlockSync(stream_t *s, uint64_t start_offset, uint64_t *out_offset) {
    if (vlc_stream_Seek(s, start_offset) != VLC_SUCCESS) return VLC_EGENERIC;

    const size_t scan_window = 65536;
    const uint8_t *p_buf;
    ssize_t n_peek = vlc_stream_Peek(s, &p_buf, scan_window);
    if (n_peek < 9) return VLC_EGENERIC;

    for (ssize_t i = 0; i + 9 <= n_peek; i++) {
        if (p_buf[i] == 0xFF && p_buf[i + 1] == 0xFF) {
            uint32_t bsize = (((uint32_t)p_buf[i + 2] << 16) |
                              ((uint32_t)p_buf[i + 3] << 8)  |
                              p_buf[i + 4]) + 5;

            // Plausible block size check
            if (bsize >= 9 && bsize <= 131072) {
                *out_offset = start_offset + (uint64_t)i;
                return VLC_SUCCESS;
            }
        }
    }
    return VLC_EGENERIC;
}

static int Control(demux_t *p_demux, int i_query, va_list args) {
    struct demux_sys_t *p_sys = p_demux->p_sys;

    switch (i_query) {
        case DEMUX_CAN_SEEK:
        {
            bool b_can_seek = false;
            if (vlc_stream_Control(p_demux->s, STREAM_CAN_SEEK, &b_can_seek) == VLC_SUCCESS) {
                *va_arg(args, bool *) = b_can_seek;
                return VLC_SUCCESS;
            }
            *va_arg(args, bool *) = (p_sys->header.metadata.seek_table != NULL);
            return VLC_SUCCESS;
        }

        case DEMUX_GET_LENGTH:
            if (p_sys->header.wave_format.sampling_rate > 0) {
                *va_arg(args, vlc_tick_t *) = dana_tick_from_samples(
                    p_sys->header.num_samples,
                    p_sys->header.wave_format.sampling_rate);
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_GET_TIME:
            if (p_sys->header.wave_format.sampling_rate > 0) {
                *va_arg(args, vlc_tick_t *) = dana_tick_from_samples(
                    p_sys->current_sample,
                    p_sys->header.wave_format.sampling_rate);
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_GET_META:
        {
            vlc_meta_t *p_meta = va_arg(args, vlc_meta_t *);
            if (!p_meta) return VLC_EGENERIC;
            if (p_sys->header.metadata.title)  vlc_meta_SetTitle(p_meta, p_sys->header.metadata.title);
            if (p_sys->header.metadata.artist) vlc_meta_SetArtist(p_meta, p_sys->header.metadata.artist);
            if (p_sys->header.metadata.album)  vlc_meta_SetAlbum(p_meta, p_sys->header.metadata.album);
            if (p_sys->header.metadata.genre)  vlc_meta_SetGenre(p_meta, p_sys->header.metadata.genre);
            if (p_sys->header.metadata.track)  vlc_meta_SetTrackNum(p_meta, p_sys->header.metadata.track);
            if (p_sys->header.metadata.year)   vlc_meta_SetDate(p_meta, p_sys->header.metadata.year);
            if (p_sys->p_art) vlc_meta_SetArtURL(p_meta, "attachment://art.jpg");
            return VLC_SUCCESS;
        }

        case DEMUX_GET_ATTACHMENTS:
        {
            if (!p_sys->p_art) return VLC_EGENERIC;
            input_attachment_t ***ppp_attach = va_arg(args, input_attachment_t ***);
            int *pi_count = va_arg(args, int *);
            *ppp_attach = malloc(sizeof(input_attachment_t *));
            if (!*ppp_attach) return VLC_ENOMEM;
            (*ppp_attach)[0] = vlc_input_attachment_Duplicate(p_sys->p_art);
            *pi_count = 1;
            return VLC_SUCCESS;
        }

        case DEMUX_SET_TIME:
        case DEMUX_SET_POSITION:
        {
            vlc_tick_t i_time;
            if (i_query == DEMUX_SET_TIME) {
                i_time = va_arg(args, vlc_tick_t);
            } else {
                double f_pos = va_arg(args, double);
                vlc_tick_t i_len = dana_tick_from_samples(p_sys->header.num_samples, p_sys->header.wave_format.sampling_rate);
                i_time = (vlc_tick_t)(f_pos * (double)i_len);
            }

            uint32_t target_sample = (uint32_t)dana_samples_from_tick(i_time, p_sys->header.wave_format.sampling_rate);
            uint64_t target_file_offset = 0;
            uint32_t seek_sample = 0;

            if (p_sys->header.metadata.seek_table) {
                // Seek via seek table
                uint32_t out_byte_offset = 0;
                if (DANADecoder_GetSeekPoint(&p_sys->header.metadata, target_sample, &seek_sample, &out_byte_offset) == DANA_APIRESULT_OK) {
                    target_file_offset = p_sys->header_size + out_byte_offset;
                } else {
                    return VLC_EGENERIC;
                }
            } else {
                // Approximate seek via sync-code scanning
                uint64_t total_size = 0;
                if (vlc_stream_GetSize(p_demux->s, &total_size) != VLC_SUCCESS || total_size <= p_sys->header_size) {
                    return VLC_EGENERIC;
                }
                uint64_t audio_bytes = total_size - p_sys->header_size;
                double ratio = (p_sys->header.num_samples > 0)
                    ? (double)target_sample / (double)p_sys->header.num_samples : 0.0;
                if (ratio > 1.0) ratio = 1.0;

                uint64_t approx_offset = p_sys->header_size + (uint64_t)(ratio * (double)audio_bytes);
                if (FindNearestBlockSync(p_demux->s, approx_offset, &target_file_offset) != VLC_SUCCESS) {
                    return VLC_EGENERIC;
                }
                seek_sample = target_sample;
            }

            if (vlc_stream_Seek(p_demux->s, target_file_offset) == VLC_SUCCESS) {
                p_sys->current_sample = seek_sample;
                p_sys->b_eof = false;

                vlc_tick_t new_pts = VLC_TICK_0 + dana_tick_from_samples(seek_sample, p_sys->header.wave_format.sampling_rate);
                es_out_Control(p_demux->out, ES_OUT_RESET_PCR);
                es_out_SetPCR(p_demux->out, new_pts);

                // Send discontinuity block to flush decoder and audio output immediately
                block_t *p_discont = block_Alloc(0);
                if (p_discont) {
                    p_discont->i_flags |= BLOCK_FLAG_DISCONTINUITY;
                    p_discont->i_pts = p_discont->i_dts = new_pts;
                    es_out_Send(p_demux->out, p_sys->p_es, p_discont);
                }

                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;
        }

        case DEMUX_GET_POSITION:
            if (p_sys->header.num_samples > 0) {
                *va_arg(args, double *) = (double)p_sys->current_sample / (double)p_sys->header.num_samples;
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        default:
            return VLC_EGENERIC;
    }
}

// Codec (decoder) implementation

struct decoder_sys_t {
    struct DANADecoder *decoder;
    struct DANAHeaderInfo header;
    int32_t *pcm_out[DANA_MAX_CHANNELS];
    uint32_t max_block_samples;
};

static int DecodeAudio(decoder_t *p_dec, block_t *p_block);
static void FlushDecoder(decoder_t *p_dec);

static int OpenDecoder(vlc_object_t *p_this) {
    decoder_t *p_dec = (decoder_t *)p_this;

    if (p_dec->fmt_in.i_codec != DANA_FOURCC) return VLC_EGENERIC;
    if (p_dec->fmt_in.i_extra < DANA_HEADER_SIZE) return VLC_EGENERIC;

    struct decoder_sys_t *p_sys = vlc_obj_malloc(p_this, sizeof(struct decoder_sys_t));
    if (!p_sys) return VLC_ENOMEM;
    memset(p_sys, 0, sizeof(struct decoder_sys_t));

    if (DANADecoder_DecodeHeader(p_dec->fmt_in.p_extra, p_dec->fmt_in.i_extra, &p_sys->header, NULL) != DANA_APIRESULT_OK) {
        return VLC_EGENERIC;
    }

    struct DANADecoderConfig cfg = {
        .max_num_channels         = p_sys->header.wave_format.num_channels,
        .max_num_block_samples    = p_sys->header.encode_param.max_num_block_samples,
        .max_parcor_order         = p_sys->header.encode_param.parcor_order,
        .max_longterm_order       = p_sys->header.encode_param.longterm_order,
        .max_lms_order_per_filter = p_sys->header.encode_param.lms_order_per_filter,
        .enable_crc_check         = 1,
        .verpose_flag             = 0,
        .num_threads              = 1
    };

    p_sys->decoder = DANADecoder_Create(&cfg);
    if (!p_sys->decoder) {
        DANAMetadata_Release(&p_sys->header.metadata);
        return VLC_EGENERIC;
    }

    DANADecoder_SetWaveFormat(p_sys->decoder, &p_sys->header.wave_format);
    DANADecoder_SetEncodeParameter(p_sys->decoder, &p_sys->header.encode_param);

    p_sys->max_block_samples = p_sys->header.encode_param.max_num_block_samples;
    for (uint32_t ch = 0; ch < p_sys->header.wave_format.num_channels; ch++) {
        p_sys->pcm_out[ch] = malloc(sizeof(int32_t) * p_sys->max_block_samples);
    }

    // Configure VLC audio output format
    p_dec->fmt_out.i_codec = (p_sys->header.wave_format.bit_per_sample == 16) ? VLC_CODEC_S16N : VLC_CODEC_S32N;
    p_dec->fmt_out.audio.i_rate = p_sys->header.wave_format.sampling_rate;
    p_dec->fmt_out.audio.i_channels = p_sys->header.wave_format.num_channels;
    p_dec->fmt_out.audio.i_bitspersample = (p_sys->header.wave_format.bit_per_sample == 16) ? 16 : 32;
    p_dec->fmt_out.audio.i_physical_channels = (p_dec->fmt_out.audio.i_channels == 1) ? AOUT_CHAN_CENTER : AOUT_CHANS_STEREO;

    p_dec->p_sys = p_sys;
    p_dec->pf_decode = DecodeAudio;
    p_dec->pf_flush = FlushDecoder;

    return VLC_SUCCESS;
}

static void CloseDecoder(vlc_object_t *p_this) {
    decoder_t *p_dec = (decoder_t *)p_this;
    struct decoder_sys_t *p_sys = p_dec->p_sys;

    for (uint32_t ch = 0; ch < p_sys->header.wave_format.num_channels; ch++) {
        free(p_sys->pcm_out[ch]);
    }
    if (p_sys->decoder) DANADecoder_Destroy(p_sys->decoder);
    DANAMetadata_Release(&p_sys->header.metadata);
}

static void FlushDecoder(decoder_t *p_dec) {
    struct decoder_sys_t *p_sys = p_dec->p_sys;
    if (p_sys && p_sys->decoder) {
        // Recreating the decoder core purges all filter history (LPC, LTP, LMS)
        struct DANADecoderConfig cfg = {
            .max_num_channels         = p_sys->header.wave_format.num_channels,
            .max_num_block_samples    = p_sys->header.encode_param.max_num_block_samples,
            .max_parcor_order         = p_sys->header.encode_param.parcor_order,
            .max_longterm_order       = p_sys->header.encode_param.longterm_order,
            .max_lms_order_per_filter = p_sys->header.encode_param.lms_order_per_filter,
            .enable_crc_check         = 1,
            .verpose_flag             = 0,
            .num_threads              = 1
        };
        DANADecoder_Destroy(p_sys->decoder);
        p_sys->decoder = DANADecoder_Create(&cfg);
        if (p_sys->decoder) {
            DANADecoder_SetWaveFormat(p_sys->decoder, &p_sys->header.wave_format);
            DANADecoder_SetEncodeParameter(p_sys->decoder, &p_sys->header.encode_param);
        }
    }
}

static int DecodeAudio(decoder_t *p_dec, block_t *p_block) {
    struct decoder_sys_t *p_sys = p_dec->p_sys;

    if (!p_block) return VLC_SUCCESS;

    if (p_block->i_flags & (BLOCK_FLAG_CORRUPTED | BLOCK_FLAG_DISCONTINUITY)) {
        FlushDecoder(p_dec);
        if (p_block->i_buffer == 0 || (p_block->i_flags & BLOCK_FLAG_CORRUPTED)) {
            block_Release(p_block);
            return VLC_SUCCESS;
        }
    }

    uint32_t out_bsize = 0, out_nsamples = 0;
    DANAApiResult ret = DANADecoder_DecodeBlock(
        p_sys->decoder,
        p_block->p_buffer,
        (uint32_t)p_block->i_buffer,
        p_sys->pcm_out,
        p_sys->max_block_samples,
        &out_bsize,
        &out_nsamples
    );

    if (ret != DANA_APIRESULT_OK || out_nsamples == 0) {
        block_Release(p_block);
        return VLC_SUCCESS;
    }

    if (decoder_UpdateAudioFormat(p_dec)) {
        block_Release(p_block);
        return VLC_SUCCESS;
    }

    uint32_t num_channels = p_sys->header.wave_format.num_channels;
    bool is_16bit = (p_sys->header.wave_format.bit_per_sample == 16);
    size_t sample_size = is_16bit ? sizeof(int16_t) : sizeof(int32_t);

    block_t *p_out = block_Alloc(out_nsamples * num_channels * sample_size);
    if (!p_out) {
        block_Release(p_block);
        return VLC_SUCCESS;
    }

    // Interleave PCM channels
    if (is_16bit) {
        int16_t *dst = (int16_t *)p_out->p_buffer;
        for (uint32_t s = 0; s < out_nsamples; s++) {
            for (uint32_t c = 0; c < num_channels; c++) {
                *dst++ = (int16_t)(p_sys->pcm_out[c][s] >> 16);
            }
        }
    } else {
        int32_t *dst = (int32_t *)p_out->p_buffer;
        for (uint32_t s = 0; s < out_nsamples; s++) {
            for (uint32_t c = 0; c < num_channels; c++) {
                *dst++ = p_sys->pcm_out[c][s];
            }
        }
    }

    p_out->i_pts = p_block->i_pts;
    p_out->i_dts = p_block->i_dts;
    p_out->i_length = dana_tick_from_samples(out_nsamples, p_sys->header.wave_format.sampling_rate);
    p_out->i_nb_samples = out_nsamples;

    block_Release(p_block);
    decoder_QueueAudio(p_dec, p_out);

    return VLC_SUCCESS;
}