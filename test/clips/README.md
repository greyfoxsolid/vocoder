# Test speech clips (dv codecs)

Real recorded speech, 8 kHz mono 16-bit PCM, from the Open Speech Repository
(Harvard sentences, American English). Free to use and publish; credit:
"Open Speech Repository".

| File | Source | Cut |
|---|---|---|
| `male_harvard_8k.wav` | `OSR_us_000_0030_8k.wav` (male) | 1.5 s to 11.2 s (9.7 s) |
| `female_harvard_8k.wav` | `OSR_us_000_0010_8k.wav` (female) | 1.0 s to 11.0 s (10 s) |

`test/dv_test.cpp` and the app's real-helper round-trip test also derive a
QUIET clip (male scaled down 20 dB) and a LOUD clip (female scaled up 12 dB,
hard-clipped like a hot microphone) from these two, so no extra files are needed.
