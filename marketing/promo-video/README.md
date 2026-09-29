# Promo video

An 88-second, 1080p30 promo video that introduces the VDO.Ninja OBS plugin.

The video is a deterministic HTML animation (`index.html`, `render(t)`). Playwright renders it frame by frame.
`music.py` synthesizes the soundtrack with numpy (120 BPM, with drops on the scene cuts).

| Time | Scene |
| --- | --- |
| 0–6s | Cold open |
| 6–12s | Title and key features |
| 12–20s | The old way (browser sources) vs. native OBS integration |
| 20–32s | Basics: pick the VDO.Ninja service, set a stream ID, then Start Streaming |
| 32–40s | Share one link: P2P fan-out to many viewers |
| 40–48s | VDO.Ninja Studio dock |
| 48–56s | Rooms and auto-inbound grid, with tally |
| 56–64s | Transparent VP9 alpha avatars (native receiver) |
| 64–74s | Power-user controls (TURN, salt, adaptive bitrate, RED, and more) |
| 74–80s | Platforms and open source |
| 80–88s | Call to action |

## Render

```bash
npm ci                      # from the repo root (provides @playwright/test)
pip install numpy
node render.mjs preview 8 26 45     # writes stills to preview/
node render.mjs full                # writes frames/00000.jpg ... (2640 frames)
python3 music.py                    # writes music.wav
ffmpeg -framerate 30 -i frames/%05d.jpg -i music.wav -c:v libx264 -preset slow -crf 22 \
  -pix_fmt yuv420p -movflags +faststart -c:a aac -b:a 192k -shortest vdoninja-obs-plugin-promo.mp4
```

The fonts are Outfit and JetBrains Mono (SIL Open Font License), bundled from Google Fonts.
