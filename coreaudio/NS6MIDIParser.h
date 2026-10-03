#ifndef NS6_MIDI_PARSER_H
#define NS6_MIDI_PARSER_H

#include <stddef.h>
#include <stdint.h>

/* The controller's 42-byte USB reports are fragments of one MIDI byte stream. */
struct ns6_midi_parser {
    uint8_t message[3];
    uint8_t used;
    uint8_t expected;
};

/* In the 42-byte NS6 report, byte 41 is a report terminator, not MIDI data. */
static inline size_t ns6_midi_report_stream_length(size_t length) {
    return length > 41 ? 41 : length;
}

static inline size_t ns6_midi_parser_feed(struct ns6_midi_parser *parser,
                                          const uint8_t *input, size_t length,
                                          uint8_t *output, size_t capacity) {
    size_t written = 0;
    if (!parser || !input || !output) return 0;
    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = input[i];
        /* 0xfd fills unused USB report bytes; it is not a MIDI message. */
        if (byte == 0xfd) continue;
        if (byte & 0x80) {
            parser->used = 0;
            parser->expected = 0;
            if (byte >= 0x80 && byte <= 0xef) {
                parser->message[0] = byte;
                parser->used = 1;
                parser->expected = (byte >= 0xc0 && byte <= 0xdf) ? 2 : 3;
            }
            continue;
        }
        if (!parser->expected || parser->used >= sizeof(parser->message)) continue;
        parser->message[parser->used++] = byte;
        if (parser->used == parser->expected) {
            if (written + parser->expected > capacity) {
                parser->used = 0;
                parser->expected = 0;
                continue;
            }
            for (unsigned j = 0; j < parser->expected; ++j)
                output[written++] = parser->message[j];
            parser->used = 0;
            parser->expected = 0;
        }
    }
    return written;
}

#endif
