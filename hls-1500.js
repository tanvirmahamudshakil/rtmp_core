import http from "k6/http";
import { check, sleep } from "k6";

const MASTER_URL =
  __ENV.URL || "http://23.19.228.53/hls/test/probe/master.m3u8";

export const options = {
  discardResponseBodies: true,

  scenarios: {
    hls_viewers: {
      executor: "ramping-vus",

      startVUs: 0,

      stages: [
        { duration: "1m", target: 100 },
        { duration: "2m", target: 300 },
        { duration: "2m", target: 500 },
        { duration: "3m", target: 1000 },
        { duration: "3m", target: 1500 },

        // 1500 users continuously watching
        { duration: __ENV.HOLD || "30m", target: 1500 },

        { duration: "2m", target: 0 },
      ],

      gracefulRampDown: "30s",
    },
  },

  thresholds: {
    http_req_failed: ["rate<0.02"],
  },
};

// ------------------------------
// Per-VU state
// ------------------------------

let mediaPlaylistUrl = null;

let seenSegments = new Set();
let seenQueue = [];

let firstPlaylistLoad = true;

// Different common real-player User Agents
const USER_AGENTS = [
  "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 Version/18.0 Mobile/15E148 Safari/604.1",

  "Mozilla/5.0 (Linux; Android 14; Pixel 8 Pro) AppleWebKit/537.36 Chrome/128.0.0.0 Mobile Safari/537.36",

  "Mozilla/5.0 (Macintosh; Apple Silicon Mac OS X 14_6) AppleWebKit/605.1.15 Version/17.6 Safari/605.1.15",
];

const USER_AGENT = USER_AGENTS[(__VU - 1) % USER_AGENTS.length];

function headers(extra = {}) {
  return Object.assign(
    {
      "User-Agent": USER_AGENT,
      Accept: "*/*",
      Connection: "keep-alive",
    },
    extra,
  );
}

function absoluteUrl(base, ref) {
  if (!ref) {
    return null;
  }

  ref = ref.trim();

  if (ref.startsWith("http://") || ref.startsWith("https://")) {
    return ref;
  }

  // /path/file.ts
  if (ref.startsWith("/")) {
    const match = base.match(/^(https?:\/\/[^/]+)/);

    if (!match) {
      return ref;
    }

    return match[1] + ref;
  }

  // relative path
  const pos = base.lastIndexOf("/");

  return base.substring(0, pos + 1) + ref;
}

function getMasterPlaylist() {
  const res = http.get(MASTER_URL, {
    headers: headers({
      Accept: "application/vnd.apple.mpegurl, application/x-mpegURL, */*",
    }),

    responseType: "text",

    tags: {
      type: "master_playlist",
    },
  });

  check(res, {
    "master playlist status 200": (r) => r.status === 200,
  });

  if (res.status !== 200 || !res.body) {
    console.error(`VU ${__VU}: master playlist failed: ${res.status}`);

    return null;
  }

  return res.body;
}

function selectVariant(masterBody) {
  const lines = masterBody
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean);

  let candidates = [];

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];

    if (line.startsWith("#EXT-X-STREAM-INF")) {
      let bandwidth = 0;

      const bwMatch = line.match(/BANDWIDTH=(\d+)/);

      if (bwMatch) {
        bandwidth = Number(bwMatch[1]);
      }

      // Next non-comment line = variant URL
      for (let j = i + 1; j < lines.length; j++) {
        if (lines[j] && !lines[j].startsWith("#")) {
          candidates.push({
            url: absoluteUrl(MASTER_URL, lines[j]),

            bandwidth: bandwidth,
          });

          break;
        }
      }
    }
  }

  // No variants means master itself may already
  // be a media playlist.
  if (candidates.length === 0) {
    if (
      masterBody.includes("#EXTINF") ||
      masterBody.includes("#EXT-X-TARGETDURATION")
    ) {
      return MASTER_URL;
    }

    return null;
  }

  // Highest available bitrate:
  // useful for maximum realistic bandwidth load.
  candidates.sort((a, b) => b.bandwidth - a.bandwidth);

  return candidates[0].url;
}

function rememberSegment(url) {
  if (seenSegments.has(url)) {
    return;
  }

  seenSegments.add(url);
  seenQueue.push(url);

  // Prevent unlimited per-VU memory growth.
  if (seenQueue.length > 100) {
    const old = seenQueue.shift();

    seenSegments.delete(old);
  }
}

function parsePlaylist(body, playlistUrl) {
  const lines = body
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean);

  const segments = [];

  const specialResources = [];

  let targetDuration = 4;

  for (const line of lines) {
    // Segment target duration
    if (line.startsWith("#EXT-X-TARGETDURATION:")) {
      const value = Number(line.split(":")[1].trim());

      if (Number.isFinite(value) && value > 0) {
        targetDuration = value;
      }
    }

    // fMP4 init segment
    if (line.startsWith("#EXT-X-MAP:")) {
      const match = line.match(/URI="([^"]+)"/);

      if (match) {
        specialResources.push(absoluteUrl(playlistUrl, match[1]));
      }
    }

    // AES key
    if (line.startsWith("#EXT-X-KEY:")) {
      const match = line.match(/URI="([^"]+)"/);

      if (match) {
        specialResources.push(absoluteUrl(playlistUrl, match[1]));
      }
    }

    // Normal TS / M4S segment
    if (!line.startsWith("#")) {
      const url = absoluteUrl(playlistUrl, line);

      if (url && !url.endsWith(".m3u8")) {
        segments.push(url);
      }
    }
  }

  return {
    segments,
    specialResources,
    targetDuration,
  };
}

function downloadResource(url, resourceType) {
  if (!url) {
    return;
  }

  const res = http.get(url, {
    headers: headers(),

    // Download data from network but
    // don't keep huge video body in JS memory.
    responseType: "none",

    tags: {
      type: resourceType,
    },

    timeout: "30s",
  });

  check(res, {
    [`${resourceType} downloaded`]: (r) => r.status >= 200 && r.status < 400,
  });
}

function loadMediaPlaylist() {
  const res = http.get(mediaPlaylistUrl, {
    headers: headers({
      Accept: "application/vnd.apple.mpegurl, application/x-mpegURL, */*",
    }),

    responseType: "text",

    tags: {
      type: "media_playlist",
    },

    timeout: "15s",
  });

  check(res, {
    "media playlist status 200": (r) => r.status === 200,
  });

  if (res.status !== 200 || !res.body) {
    sleep(2);

    return;
  }

  const parsed = parsePlaylist(res.body, mediaPlaylistUrl);

  // init.mp4 / encryption key
  for (const url of parsed.specialResources) {
    if (!seenSegments.has(url)) {
      downloadResource(url, "hls_resource");

      rememberSegment(url);
    }
  }

  let segments = parsed.segments;

  /*
   * A new live viewer normally joins
   * around the live edge instead of
   * downloading the entire historical list.
   */
  if (firstPlaylistLoad) {
    segments = segments.slice(-3);

    firstPlaylistLoad = false;
  }

  // Download each new segment once per viewer
  for (const segment of segments) {
    if (!seenSegments.has(segment)) {
      downloadResource(segment, "video_segment");

      rememberSegment(segment);
    }
  }

  /*
   * Refresh playlist based roughly
   * on HLS target duration.
   */
  let refresh = parsed.targetDuration / 2;

  if (refresh < 1) {
    refresh = 1;
  }

  if (refresh > 5) {
    refresh = 5;
  }

  sleep(refresh);
}

export default function () {
  /*
   * One-time initialization
   * for each virtual viewer.
   */

  if (!mediaPlaylistUrl) {
    const master = getMasterPlaylist();

    if (!master) {
      sleep(3);

      return;
    }

    mediaPlaylistUrl = selectVariant(master);

    if (!mediaPlaylistUrl) {
      console.error(`VU ${__VU}: no HLS media playlist found`);

      sleep(5);

      return;
    }
  }

  /*
   * Continuous watching
   */

  loadMediaPlaylist();
}
