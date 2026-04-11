// Get decoded frame from AOM
aom_image_t *img = aom_codec_get_frame(&decoder->aom_decoder, &decoder->aom_decoder.iter);

if (img) {
    // Process single frame...
}
