#pragma once

// Dispatch the MIDI command family.
// The outer console has already consumed "midi".
void console_midi_command(const char *args);

// Foreground-only diagnostic work: at most 32 automatic verification events.
// Does not print, wait, delay or service USB recursively.
void console_midi_service(void);
