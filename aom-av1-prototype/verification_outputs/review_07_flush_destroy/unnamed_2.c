av1_mem_set_override_enabled(false);
av1_mem_shutdown();
free(decoder);  // WRONG! Should use av1_mem_free()
