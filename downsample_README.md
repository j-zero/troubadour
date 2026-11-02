# downsample.py

Resamples every sample above 48 kHz in a SoundFont (SF2) to 48 kHz. Instruments with high-resolution samples then
need much less RAM, with no audible change to the sound: mt32-pi outputs at 48 kHz anyway, so everything above
24 kHz is lost regardless.

Intended for the Raspberry Pi Zero 2 W with dynamic sample loading (`dynamic_samples = on`): there, the selected
instrument has to fit into RAM completely (about 440 MB).

## Requirements

- Python 3
- numpy and scipy (`pip install numpy scipy`, or on Ubuntu/WSL `sudo apt install python3-numpy python3-scipy`)
- Free space for the new file and for a temporary file with the sample data (each about the size of the new
  SoundFont)

## Usage

```sh
python3 downsample.py <input.sf2> <output.sf2>
```

Example (quotes because of the parentheses in the file name):

```sh
python3 downsample.py "Realistic_Soundfont_V2_Libre_V1_(16bit)Sub2GiB_XGMS_naturalFixed.sf2" new.sf2
```

The input file is not changed. So that the last selected SoundFont in `last_state.cfg` stays valid, put the new
file into the `soundfonts` folder of the SD card under the original name afterwards, and keep the original
somewhere else.

## What is changed

- Only samples with a sample rate above 48 kHz are resampled: with a polyphase low-pass filter
  (`scipy.signal.resample_poly`, Kaiser window), then rounded and clipped to 16 bits.
- The loop points of these samples are converted to the new rate (rounded to whole samples), and the sample rate
  in the sample header is set to 48000. Root key and tuning stay the same.
- All other samples, instruments, presets and info are copied unchanged. As required by the SF2 specification,
  46 zero samples are inserted after each sample.

The target rate is set by `TARGET` at the top of the script.

## Limitations

- 16-bit SF2 files only: SoundFonts with 24-bit data (`sm24`) and SF3 (compressed) are not supported.
- Samples whose data overlaps in the file are rejected (the script stops).
- With very short loops, rounding the loop points can make the loop transition slightly audible.

## Result with the Realistic SoundFont

`Realistic_Soundfont_V2_Libre_V1_(16bit)Sub2GiB_XGMS_naturalFixed.sf2`: 176 samples of Riky's Grand Piano were
stored at 96 kHz.

| | before | after |
|---|---|---|
| Riky's Grand Piano | 516 MB (doesn't fit into RAM) | 324 MB |
| Largest instrument | 516 MB | 349 MB (Acoustic KSP) |
| SoundFont file | 1987 MB | 1795 MB |

Checked with FluidSynth 2.6.2 and a memory limit like on the Pi: the piano then loads completely. Rendered notes
have the same pitch and level; the difference from the original is 61–72 dB below the signal.
