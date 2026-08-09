import http from "k6/http";
import { check, sleep } from "k6";
import { Counter, Rate } from "k6/metrics";

const MASTER_URL =
  __ENV.URL || "http://23.19.228.53/hls/test/probe/master.m3u8";

const HOLD_TIME = __ENV.HOLD || "30m";
const QUALITY = (__ENV.QUALITY || "auto").toLowerCase();

export const segmentDownloads = new Counter("hls_segments_downloaded");
export const streamErrors = new Rate("hls_stream_errors");

export const options = {
  // Video segments network দিয়ে download হবে,
  // কিন্তু JS memory-তে body রাখা হবে না।
  discardResponseBodies: true,

  scenarios: {
    viewers: {
      executor: "ramping-vus",

      startVUs: 0,

      stages: [
        // Gradually connect viewers
        { duration: "1m", target: 100 },
        { duration: "2m", target: 300 },
        { duration: "2m", target: 500 },
        { duration: "3m", target: 800 },
        { duration: "3m", target: 1000 },
        { duration: "3m", target: 1250 },
        { duration: "3m", target: 1500 },

        // All 1500 viewers continue watching
        { duration: HOLD_TIME, target: 1500 },

        // Graceful disconnect
        { duration: "2m", target: 0 },
      ],

      gracefulRampDown: "30s",
    },
  },

  thresholds: {
    http_req_failed: ["rate<0.05"],
    hls_stream_errors: ["rate<0.05"],
  },
};

// =====================================================
// Per virtual-user state
// =====================================================

let mediaPlaylistURL = null;
let initialized = false;

const downloadedResources = new Set();
const resourceQueue = [];

const USER_AGENTS = [
  "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 Version/18.0 Mobile/15E148 Safari/604.1",

  "Mozilla/5.0 (Linux; Android 14; Pixel 8 Pro) AppleWebKit/537.36 Chrome/128.0.0.0 Mobile Safari/537.36",

  "Mozilla/5.0 (Linux; Android 13; SM-S918B) AppleWebKit/537.36 Chrome/126.0.0.0 Mobile Safari/537.36",

  "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_6) AppleWebKit/605.1.15 Version/17.6 Safari/605.1.15",
];

// Each VU keeps same UA during its session.
const viewerUserAgent = USER_AGENTS[(__VU - 1) % USER_AGENTS.length];

function playlistHeaders() {
  return {
    "User-Agent": viewerUserAgent,
    Accept: "application/vnd.apple.mpegurl, application/x-mpegURL, */*",
    "Cache-Control": "no-cache",
    Pragma: "no-cache",
  };
}

function segmentHeaders() {
  return {
    "User-Agent": viewerUserAgent,
    Accept: "*/*",
  };
}

// =====================================================
// URL resolver
// =====================================================

function resolveURL(base, ref) {
  if (!ref) {
    return null;
  }

  ref = ref.trim();

  if (ref.startsWith("http://") || ref.startsWith("https://")) {
    return ref;
  }

  const originMatch = base.match(/^(https?:\/\/[^/]+)/);

  if (!originMatch) {
    return ref;
  }

  const origin = originMatch[1];

  if (ref.startsWith("/")) {
    return origin + ref;
  }

  const baseWithoutQuery = base.split("?")[0];

  const slash = baseWithoutQuery.lastIndexOf("/");

  return baseWithoutQuery.substring(0, slash + 1) + ref;
}

// =====================================================
// Master playlist
// =====================================================

function fetchMasterPlaylist() {
  const response = http.get(MASTER_URL, {
    headers: playlistHeaders(),

    // We need playlist text.
    responseType: "text",

    timeout: "15s",

    tags: {
      request_type: "master_playlist",
    },
  });

  const ok = check(response, {
    "master playlist loaded": (r) => r.status === 200 && !!r.body,
  });

  streamErrors.add(!ok);

  if (!ok) {
    return null;
  }

  return response.body;
}

// =====================================================
// Variant selection
// =====================================================

function selectVariant(master) {
  const lines = master
    .split(/\r?\n/)
    .map((x) => x.trim())
    .filter((x) => x.length > 0);

  const variants = [];

  for (let i = 0; i < lines.length; i++) {
    if (!lines[i].startsWith("#EXT-X-STREAM-INF")) {
      continue;
    }

    let bandwidth = 0;

    const bandwidthMatch = lines[i].match(/BANDWIDTH=(\d+)/);

    if (bandwidthMatch) {
      bandwidth = Number(bandwidthMatch[1]);
    }

    for (let next = i + 1; next < lines.length; next++) {
      if (!lines[next].startsWith("#")) {
        variants.push({
          url: resolveURL(MASTER_URL, lines[next]),

          bandwidth,
        });

        break;
      }
    }
  }

  // Master URL itself may already be
  // a media playlist.
  if (variants.length === 0) {
    if (
      master.includes("#EXTINF") ||
      master.includes("#EXT-X-TARGETDURATION")
    ) {
      return MASTER_URL;
    }

    return null;
  }

  variants.sort((a, b) => a.bandwidth - b.bandwidth);

  // Force quality if requested.
  if (QUALITY === "low") {
    return variants[0].url;
  }

  if (QUALITY === "high") {
    return variants[variants.length - 1].url;
  }

  if (QUALITY === "mid") {
    return variants[Math.floor(variants.length / 2)].url;
  }

  /*
   * AUTO:
   *
   * Make viewer distribution more realistic.
   *
   * ~25% low
   * ~50% middle
   * ~25% high
   */

  const bucket = __VU % 4;

  if (bucket === 0) {
    return variants[0].url;
  }

  if (bucket === 3 && variants.length > 1) {
    return variants[variants.length - 1].url;
  }

  return variants[Math.floor(variants.length / 2)].url;
}

// =====================================================
// Resource tracking
// =====================================================

function hasResource(url) {
  return downloadedResources.has(url);
}

function rememberResource(url) {
  downloadedResources.add(url);
  resourceQueue.push(url);

  /*
   * Don't let the Set grow forever.
   * Live streams can run for hours.
   */
  if (resourceQueue.length > 200) {
    const old = resourceQueue.shift();

    downloadedResources.delete(old);
  }
}

// =====================================================
// Media playlist parser
// =====================================================

function parseMediaPlaylist(body, playlistURL) {
  const lines = body
    .split(/\r?\n/)
    .map((x) => x.trim())
    .filter(Boolean);

  let targetDuration = 4;

  const segments = [];
  const extraResources = [];

  for (const line of lines) {
    // -----------------------------
    // Playlist refresh interval
    // -----------------------------

    if (line.startsWith("#EXT-X-TARGETDURATION:")) {
      const n = Number(line.substring("#EXT-X-TARGETDURATION:".length));

      if (Number.isFinite(n) && n > 0) {
        targetDuration = n;
      }

      continue;
    }

    // -----------------------------
    // fMP4 init file
    // -----------------------------

    if (line.startsWith("#EXT-X-MAP:")) {
      const match = line.match(/URI="([^"]+)"/);

      if (match) {
        extraResources.push(resolveURL(playlistURL, match[1]));
      }

      continue;
    }

    // -----------------------------
    // Encryption key
    // -----------------------------

    if (line.startsWith("#EXT-X-KEY:")) {
      const match = line.match(/URI="([^"]+)"/);

      if (match) {
        extraResources.push(resolveURL(playlistURL, match[1]));
      }

      continue;
    }

    // Ignore HLS metadata.
    if (line.startsWith("#")) {
      continue;
    }

    const url = resolveURL(playlistURL, line);

    // Nested playlist shouldn't be
    // downloaded as a video segment.
    if (url && !url.includes(".m3u8")) {
      segments.push(url);
    }
  }

  return {
    targetDuration,
    segments,
    extraResources,
  };
}

// =====================================================
// Segment download
// =====================================================

function downloadSegment(url, type = "video_segment") {
  if (!url) {
    return false;
  }

  const response = http.get(url, {
    headers: segmentHeaders(),

    /*
     * Body is fully transferred over network
     * but not retained by k6 JS.
     */
    responseType: "none",

    timeout: "30s",

    tags: {
      request_type: type,
    },
  });

  const ok = check(response, {
    "HLS resource downloaded": (r) => r.status >= 200 && r.status < 400,
  });

  streamErrors.add(!ok);

  if (ok) {
    segmentDownloads.add(1);
  }

  return ok;
}

// =====================================================
// Viewer initialization
// =====================================================

function initializeViewer() {
  const master = fetchMasterPlaylist();

  if (!master) {
    return false;
  }

  mediaPlaylistURL = selectVariant(master);

  if (!mediaPlaylistURL) {
    console.error(`VU ${__VU}: no media playlist found`);

    streamErrors.add(true);

    return false;
  }

  initialized = true;

  return true;
}

// =====================================================
// Continuous playback cycle
// =====================================================

function watchStream() {
  const response = http.get(mediaPlaylistURL, {
    headers: playlistHeaders(),

    responseType: "text",

    timeout: "15s",

    tags: {
      request_type: "media_playlist",
    },
  });

  const playlistOK = check(response, {
    "media playlist loaded": (r) => r.status === 200 && !!r.body,
  });

  streamErrors.add(!playlistOK);

  if (!playlistOK) {
    sleep(2);
    return;
  }

  const playlist = parseMediaPlaylist(response.body, mediaPlaylistURL);

  // -----------------------------
  // Init files / encryption keys
  // -----------------------------

  for (const resource of playlist.extraResources) {
    if (!hasResource(resource)) {
      downloadSegment(resource, "hls_extra_resource");

      rememberResource(resource);
    }
  }

  // -----------------------------
  // Video segments
  // -----------------------------

  let segments = playlist.segments;

  /*
   * Real live player normally starts
   * near live edge rather than downloading
   * entire historical playlist.
   */
  if (downloadedResources.size === 0 && segments.length > 3) {
    segments = segments.slice(-3);
  }

  for (const segment of segments) {
    if (hasResource(segment)) {
      continue;
    }

    const success = downloadSegment(segment, "video_segment");

    if (success) {
      rememberResource(segment);
    }
  }

  /*
   * Real HLS clients continuously
   * refresh media playlist.
   *
   * Half target duration is a
   * reasonable live refresh interval.
   */

  let refreshInterval = playlist.targetDuration / 2;

  if (refreshInterval < 1) {
    refreshInterval = 1;
  }

  if (refreshInterval > 5) {
    refreshInterval = 5;
  }

  sleep(refreshInterval);
}

// =====================================================
// Each VU behaves as one continuous viewer
// =====================================================

export default function () {
  if (!initialized) {
    /*
     * Small natural join jitter so users
     * don't all request at exactly same ms.
     */

    sleep(Math.random() * 1.5);

    if (!initializeViewer()) {
      sleep(3);
      return;
    }
  }

  /*
   * One iteration = next playback cycle.
   *
   * k6 will immediately run this same VU
   * again, preserving per-VU state.
   *
   * Therefore:
   *
   * Viewer 1 -> keeps watching
   * Viewer 2 -> keeps watching
   * ...
   * Viewer 1500 -> keeps watching
   */

  watchStream();
}
