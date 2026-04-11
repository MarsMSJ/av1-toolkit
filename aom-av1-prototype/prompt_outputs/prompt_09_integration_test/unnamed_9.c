
## Error Handling

The test program includes error path tests:

- **Truncated bitstream** - Handles incomplete frames gracefully
- **Zero-length AU** - Returns AV1_INVALID_PARAM
- **QUEUE_FULL recovery** - Drains queue and continues decoding

## License

This is test code for the AV1 decoder API implementation.
