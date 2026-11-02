# downsample.py

Rechnet in einer SoundFont (SF2) alle Samples mit mehr als 48 kHz auf 48 kHz herunter. Dadurch brauchen
Instrumente mit hochaufgelösten Samples deutlich weniger RAM, ohne dass sich am Klang etwas hörbar ändert:
mt32-pi gibt ohnehin mit 48 kHz aus, alles oberhalb von 24 kHz geht also sowieso verloren.

Gedacht für den Raspberry Pi Zero 2 W mit dynamischem Sample-Laden (`dynamic_samples = on`): Dort muss jeweils
das gewählte Instrument komplett in den RAM passen (etwa 440 MB).

## Voraussetzungen

- Python 3
- numpy und scipy (`pip install numpy scipy`, unter Ubuntu/WSL auch `sudo apt install python3-numpy python3-scipy`)
- Freier Platz für die neue Datei und eine temporäre Datei mit den Sampledaten (jeweils etwa so groß wie die
  neue SoundFont)

## Aufruf

```sh
python3 downsample.py <Eingabe.sf2> <Ausgabe.sf2>
```

Beispiel (Anführungszeichen wegen der Klammern im Dateinamen):

```sh
python3 downsample.py "Realistic_Soundfont_V2_Libre_V1_(16bit)Sub2GiB_XGMS_naturalFixed.sf2" neu.sf2
```

Die Eingabedatei wird nicht verändert. Damit die zuletzt gewählte SoundFont in `last_state.cfg` gültig bleibt,
die neue Datei anschließend unter dem ursprünglichen Namen in den Ordner `soundfonts` der SD-Karte legen und das
Original woanders aufheben.

## Was geändert wird

- Nur Samples mit einer Abtastrate über 48 kHz werden umgerechnet: mit einem Polyphasen-Tiefpassfilter
  (`scipy.signal.resample_poly`, Kaiser-Fenster), danach auf 16 Bit gerundet und begrenzt.
- Die Loop-Punkte dieser Samples werden auf die neue Rate umgerechnet (auf ganze Samples gerundet), die
  Abtastrate im Sample-Header auf 48000 gesetzt. Grundton und Stimmung bleiben gleich.
- Alle anderen Samples, Instrumente, Presets und Infos werden unverändert übernommen. Nach jedem Sample werden
  wie vom SF2-Standard verlangt 46 Null-Samples eingefügt.

Die Zielrate steht in `TARGET` am Anfang des Skripts.

## Einschränkungen

- Nur 16-Bit-SF2-Dateien: SoundFonts mit 24-Bit-Daten (`sm24`) und SF3 (komprimiert) werden nicht unterstützt.
- Samples, deren Daten sich in der Datei überlappen, werden abgelehnt (das Skript bricht dann ab).
- Bei sehr kurzen Loops kann das Runden der Loop-Punkte zu einem leicht hörbaren Übergang führen.

## Ergebnis bei der Realistic SoundFont

`Realistic_Soundfont_V2_Libre_V1_(16bit)Sub2GiB_XGMS_naturalFixed.sf2`: 176 Samples von Riky's Grand Piano
lagen mit 96 kHz vor.

| | vorher | nachher |
|---|---|---|
| Riky's Grand Piano | 516 MB (passt nicht in den RAM) | 324 MB |
| Größtes Instrument | 516 MB | 349 MB (Acoustic KSP) |
| SoundFont-Datei | 1987 MB | 1795 MB |

Mit FluidSynth 2.6.2 und einer Speichergrenze wie auf dem Pi geprüft: Das Piano lädt danach vollständig. Gerenderte
Töne haben dieselbe Tonhöhe und denselben Pegel; der Unterschied zum Original liegt 61–72 dB unter dem Signal.
