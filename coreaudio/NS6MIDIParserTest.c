#include "NS6MIDIParser.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    struct ns6_midi_parser parser = {0};
    unsigned char output[32];
    /* The first report ends after CC status; the next begins with its data bytes. */
    unsigned char first[42], second[42];
    for (unsigned i = 0; i < 41; ++i) first[i] = second[i] = 0xfd;
    first[0] = 0xb1; first[41] = 0x00; /* report terminator */
    second[0] = 0x00; second[1] = 0x6e; second[2] = 0xb1; second[3] = 0x20; second[4] = 0x37; second[41] = 0x00;
    size_t first_length = ns6_midi_report_stream_length(sizeof(first));
    size_t count = ns6_midi_parser_feed(&parser, first, first_length, output, sizeof(output));
    assert(count == 0);
    count = ns6_midi_parser_feed(&parser, second, ns6_midi_report_stream_length(sizeof(second)), output, sizeof(output));
    assert(count == 6);
    assert(output[0] == 0xb1 && output[1] == 0x00 && output[2] == 0x6e);
    assert(output[3] == 0xb1 && output[4] == 0x20 && output[5] == 0x37);

    /* A complete Note Off whose velocity is zero must not be mistaken for padding. */
    const unsigned char note_off[] = {0x81, 0x23, 0x00, 0xfd};
    count = ns6_midi_parser_feed(&parser, note_off, sizeof(note_off), output, sizeof(output));
    assert(count == 3);
    assert(output[0] == 0x81 && output[1] == 0x23 && output[2] == 0x00);

    puts("NS6 MIDI parser tests passed");
    return 0;
}
