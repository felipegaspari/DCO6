#ifndef __MIDI_H_H__
#define __MIDI_H_H__

#include <MIDI.h>

Adafruit_USBD_MIDI usb_midi;

MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDI_USB);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI_SERIAL);

void init_midi();
// Clear mono last-note held stack (called from setVoiceMode when entering mono).
void mono_note_stack_clear();

#endif

