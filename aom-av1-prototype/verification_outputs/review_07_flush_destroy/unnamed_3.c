// Current code - iter is not reset
while ((img = aom_codec_get_frame(&decoder->aom_decoder, &decoder->aom_decoder.iter)) != NULL) {
