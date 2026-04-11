decoder->aom_decoder.iter = 0;  // Reset iterator
while ((img = aom_codec_get_frame(&decoder->aom_decoder, &decoder->aom_decoder.iter)) != NULL) {
