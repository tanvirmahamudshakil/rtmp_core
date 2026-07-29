# Independent transcoding

StreamForge keeps codec work outside the io_uring origin. A successful source
publish enqueues a lifecycle event; a dedicated C++ supervisor thread starts
one independent media worker for that source. One worker opens the source once
and maps it to every configured output, so adding viewers never adds encoders.

```text
RTMP source
    |
C++ io_uring origin (ingest/fanout only)
    |
C++ TranscoderSupervisor control queue
    |
independent libav/FFmpeg media process
    +-- libx264/libx265 software codecs
    +-- NVIDIA NVENC
    +-- Intel oneVPL/QuickSync
    +-- Beamr/MainConcept adapter slots (licensed SDK required)
    |
RTMP rendition publish
    |
C++ RTMP fanout + shared HLS segment store
```

The supervisor never invokes a shell. It uses `posix_spawn` with an argument
vector, bounds active jobs and outputs per job, stops workers with TERM/KILL
deadlines, and applies a finite restart budget. A codec or GPU-driver failure
therefore cannot terminate the origin process.

## Configure a rendition

The installer places the validated rules file at:

```text
/etc/rtmp-server/transcoding.conf
```

Each non-comment line contains:

```text
source|preset|output|backend|video-codec|video-bitrate|profile|keyframes|width|height|fit-mode|audio-codec|audio-bitrate|gpu|description
```

`source` may be `application/*`. Use `{source}` in the output name for a
collision-free per-source rendition, for example `football/*` with
`{source}_720p`.

For the `football/live2` source:

```text
football/live2|720p|live2_720p|default|h264|2500000|high|60|1280|720|letterbox|aac|128000|first|HD output
football/live2|480p|live2_480p|default|h264|900000|main|60|854|480|letterbox|aac|96000|first|Mobile output
```

After changing the file, restart the service. Rules are loaded and fully
validated before the RTMP listener starts.

When `rtmp://HOST:1935/football/live2` starts publishing, the supervisor
automatically creates and publishes these rendition streams:

```text
rtmp://HOST:1935/football/live2_720p
rtmp://HOST:1935/football/live2_480p
```

Their HLS media playlists are:

```text
http://HOST/hls/football/live2_720p/index.m3u8
http://HOST/hls/football/live2_480p/index.m3u8
```

The adaptive master playlist is:

```text
http://HOST/hls/football/live2/master.m3u8
```

## Admin workflow

1. Create a template from **Transcoding** in the sidebar and add one or more
   encoding presets.
2. Open **Applications**, select an application, then open its
   **Transcoding** tab.
3. Choose the source stream and template, then select **Add template**.
4. Copy the displayed **MASTER M3U8** URL. Every preset in that template is
   exposed as a rendition inside this one adaptive HLS link.

Assignments are stored in SQLite and restored when the service restarts. If the
source is already live, applying or updating a template restarts only its
transcoder worker; the publisher does not need to reconnect. Removing an
assignment stops that worker and removes the persisted rule.

Template drafts are kept in the admin browser. The complete applied rule and
its selected template name are persisted by the server, so active assignments
and their adaptive playback URLs survive an admin-browser refresh or server
restart.

## Backend behavior

- `default`/`software`: `libx264` for H.264 and the libav AAC encoder.
- `nvenc`: `h264_nvenc`; optional `gpu` selects the NVIDIA device.
- `quicksync`: `h264_qsv`, backed by Intel QuickSync/oneVPL.
- `beamr` and `mainconcept`: fail closed until their licensed SDK adapter is
  installed. They never silently fall back to software.

The preset model already represents H.263, H.265, VP8, VP9, Vorbis and Opus,
but the current RTMP-to-MPEG-TS origin accepts only H.264/AAC renditions.
Unsupported combinations are rejected before any worker is launched.

Viewer scale remains independent of transcoding scale: each preset is encoded
once, while every viewer shares immutable RTMP frames or HLS segment bytes.
Available outbound bandwidth/CDN capacity—not the encoder—sets the final
viewer ceiling.
