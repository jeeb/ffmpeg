FATE_TRUEHD-$(call DEMDEC, TRUEHD, TRUEHD) += fate-truehd-5.1
fate-truehd-5.1: CMD = md5pipe -f truehd -i $(TARGET_SAMPLES)/lossless-audio/truehd_5.1.raw -f s32le
fate-truehd-5.1: CMP = oneline
fate-truehd-5.1: REF = 95d8aac39dd9f0d7fb83dc7b6f88df35

FATE_TRUEHD-$(call DEMDEC, TRUEHD, TRUEHD) += fate-truehd-5.1-downmix-2.0
fate-truehd-5.1-downmix-2.0: CMD = md5pipe -f truehd -request_channel_layout FL+FR -i $(TARGET_SAMPLES)/lossless-audio/truehd_5.1.raw -f s32le
fate-truehd-5.1-downmix-2.0: CMP = oneline
fate-truehd-5.1-downmix-2.0: REF = a269aee0051d4400c9117136f08c9767

FATE_TRUEHD-$(call ALLYES, TRUEHD_DEMUXER TRUEHD_MUXER TRUEHD_CORE_BSF) += fate-truehd-core-bsf
fate-truehd-core-bsf: CMD = md5pipe -i $(TARGET_SAMPLES)/truehd/atmos.thd -c:a copy -bsf:a truehd_core -fflags +bitexact -f truehd
fate-truehd-core-bsf: CMP = oneline
fate-truehd-core-bsf: REF = 3aa5d0c7825051f3657b71fd6135183b

# Tests that the result from reading a copyinkf remux with the first random
# access point dropped will receive the correct timestamp for the first packet,
# which is not the packet of the first packet read.
FATE_TRUEHD-$(call ALLYES, FILE_PROTOCOL PIPE_PROTOCOL TRUEHD_DEMUXER \
                           MATROSKA_DEMUXER MLP_PARSER MATROSKA_MUXER \
                           NOISE_BSF) \
                           += fate-truehd-parser-timestamps
fate-truehd-parser-timestamps: CMD = stream_remux "truehd" \
    "$(TARGET_SAMPLES)/lossless-audio/truehd_5.1.raw" "matroska" \
    "-map 0:a -copyinkf -bsf:a noise=drop=not\(n\)*key -t 0.030" \
    "-c copy -copyts"
fate-truehd-parser-timestamps: CMP = diff

FATE_SAMPLES_AUDIO += $(FATE_TRUEHD-yes)
fate-truehd: $(FATE_TRUEHD-yes)
