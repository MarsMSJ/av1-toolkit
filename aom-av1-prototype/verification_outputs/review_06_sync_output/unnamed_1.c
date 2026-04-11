// For now, use the AOM image directly if available
if (img->planes[0]) {
    // Copy Y plane
    int y_size = img->stride[0] * img->h;
    memcpy(slot->planes[0], img->planes[0], y_size);
}
// U and V planes are NEVER copied!
