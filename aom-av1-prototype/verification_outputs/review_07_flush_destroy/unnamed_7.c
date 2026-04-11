if (decoder->workers[i].running) {
    pthread_cancel(decoder->workers[i].thread);
}
pthread_join(decoder->workers[i].thread, NULL);
