export type Application = {
  name: string;
  enabled: boolean;
};

// One core::ServerConfig field as described by settings_schema() and
// rendered by GET /v1/settings. `value` is present for non-sensitive fields;
// sensitive fields (secrets) only ever report `has_value`, never the secret
// itself. Every field takes effect only after the process is restarted --
// there is no live field here because nothing on the backend is live.
export type SettingField = {
  key: string;
  section: string;
  label: string;
  description: string;
  type: "string" | "bool" | "u16" | "u32" | "u64" | "duration_ms" | "percent";
  restart_required: boolean;
  sensitive: boolean;
  value?: string;
  has_value?: boolean;
};

export type Stream = {
  application: string;
  name: string;
  enabled: boolean;
  recording_enabled: boolean;
  rtmp_url: string;
  hls_path: string;
  is_live?: boolean;
  viewer_count?: number;
  // Cumulative bytes delivered to viewers of this stream, as of this
  // snapshot. Monotonic while live; the caller derives a per-link bitrate
  // from the delta between two snapshots (see bandwidthFromSamples in App.tsx).
  egress_bytes_total?: number;
  rtmp_egress_bytes_total?: number;
  rtmp_viewer_count?: number;
  hls_viewer_count?: number;
  // Present only when the server itself read the cache edge's accounting
  // (control::EdgeViewerStats). When it is, viewer_count is already the real
  // total and this client must not add its own edge reading on top of it.
  hls_viewers_measured?: boolean;
  hls_egress_bytes_total?: number;
  hls_egress_bitrate_bps?: number;
};

export type StreamSetup = Stream;

export type TranscodingOutput = {
  name: string;
  stream: string;
  video_codec: string;
  video_bitrate: number;
  width: number;
  height: number;
  // Delivery measured at the cache edge for this rendition's own link, so a
  // ladder shows which rung its audience is actually on. Absent on a server
  // that does not read the edge accounting.
  viewer_count?: number;
  bytes_total?: number;
  hls_egress_bitrate_bps?: number;
};

// A transcode driven from an external source URL (RTMP or HLS/TS carrying
// H.264/AAC) rather than from a stream published to this origin. The source is
// pulled, transcoded per the chosen template, and re-served as one adaptive
// master .m3u8 with a media playlist per rendition.
export type SourceTranscodeJob = {
  id: string;
  application: string;
  name: string;
  source_url: string;
  template_name: string;
  master_hls_path: string;
  outputs: TranscodingOutput[];
  status: "starting" | "running" | "error" | "stopped" | "disabled";
  detail?: string;
  enabled: boolean;
  // When the puller errors out (source unreachable, dropped mid-stream), the
  // server retries automatically after restart_delay_seconds if auto_restart
  // is set — configured at creation time.
  auto_restart: boolean;
  restart_delay_seconds: number;
  // Live delivery stats derived server-side from actual HLS playlist/segment
  // request traffic, summed across every rendition (see
  // HlsHttpHandler::aggregate_link_stats in the backend) — the only signal
  // available for a source-transcode job, since its renditions never
  // register with the RTMP viewer-tracking path. bytes_total is cumulative;
  // the caller derives a bitrate from the delta between two polls, same as
  // Stream.egress_bytes_total. viewer_count is already a live estimate
  // (distinct playback sessions seen in the last ~20s), not cumulative.
  bytes_total?: number;
  viewer_count?: number;
  hls_egress_bitrate_bps?: number;
  // True when these numbers came from the cache edge (whether the server read
  // it or this client did). False means they are the origin's own view, which
  // undercounts badly behind Varnish.
  delivery_stats_available?: boolean;
};

// A transcoding template as persisted server-side (SQLite): a name plus its
// list of encoding presets. `presets` is opaque to this client — App.tsx owns
// the EncodingPreset shape — it is only threaded through untouched.
export type TemplateRecord = {
  id: string;
  name: string;
  presets: unknown[];
};

export type Snapshot = {
  applications: Application[];
  streams: Stream[];
  metrics: Record<string, number>;
  health: "online" | "degraded" | "offline";
};

type ListResponse<T> = { items: T[]; total: number };

type EdgeDeliveryStats = {
  generated_at: number;
  window_seconds: number;
  viewers: Record<string, number>;
  bytes_total: Record<string, number>;
  bitrate_bps: Record<string, number>;
  totals: {
    viewers: number;
    bytes_total: number;
    bitrate_bps: number;
  };
};

// Varnish sees every HLS request, including cache hits. The edge estimator
// republishes active playback-session counts keyed directly by the master
// link (e.g. "kk/RRR") at /internal/viewer_estimate.json. Session identity,
// unlike IP identity, counts multiple VLC players behind one NAT separately.
// Include the master key emitted by current servers. During a rolling upgrade,
// older sessions may still be keyed by exact rendition output names; use the
// job's declared outputs rather than a name prefix so similarly named jobs
// (for example "news" and "news_backup") can never absorb each other's data.
function sourceEdgeValue(
  values: Record<string, number>,
  application: string,
  name: string,
  outputs: TranscodingOutput[]
): { matched: boolean; value: number } {
  let sum = 0;
  let matched = false;
  const streamNames = new Set([name, ...outputs.map((output) => output.stream)]);
  for (const streamName of streamNames) {
    const key = `${application}/${streamName}`;
    if (Object.hasOwn(values, key)) {
      sum += values[key];
      matched = true;
    }
  }
  return { matched, value: sum };
}

function exactEdgeValue(values: Record<string, number>, application: string, name: string): number {
  return values[`${application}/${name}`] ?? 0;
}

export class ApiError extends Error {
  constructor(
    message: string,
    public status: number,
    public requestId?: string
  ) {
    super(message);
  }
}

let demoApps: Application[] = [
  { name: "live", enabled: true },
  { name: "events", enabled: true },
  { name: "backup", enabled: false }
];

const demoRtmpUrl = (application: string, name: string) => `rtmp://stream.example.com:1935/${application}/${name}`;

let demoStreams: Stream[] = [
  { application: "live", name: "main-stage", enabled: true, recording_enabled: true, rtmp_url: demoRtmpUrl("live", "main-stage"), hls_path: "/hls/live/main-stage/index.m3u8", is_live: true, viewer_count: 3842 },
  { application: "live", name: "sports-east", enabled: true, recording_enabled: false, rtmp_url: demoRtmpUrl("live", "sports-east"), hls_path: "/hls/live/sports-east/index.m3u8", is_live: true, viewer_count: 1947 },
  { application: "events", name: "conference-a", enabled: true, recording_enabled: true, rtmp_url: demoRtmpUrl("events", "conference-a"), hls_path: "/hls/events/conference-a/index.m3u8", is_live: true, viewer_count: 826 },
  { application: "events", name: "studio-feed", enabled: true, recording_enabled: false, rtmp_url: demoRtmpUrl("events", "studio-feed"), hls_path: "/hls/events/studio-feed/index.m3u8", is_live: false, viewer_count: 0 },
  { application: "backup", name: "disaster-recovery", enabled: false, recording_enabled: false, rtmp_url: demoRtmpUrl("backup", "disaster-recovery"), hls_path: "/hls/backup/disaster-recovery/index.m3u8", is_live: false, viewer_count: 0 }
];
let demoTemplates: TemplateRecord[] = [];
let demoSourceJobs: SourceTranscodeJob[] = [];

const demoMetrics: Record<string, number> = {
  active_connections: 6624,
  active_publishers: 3,
  active_viewers: 6615,
  active_streams: 3,
  ingress_bitrate: 24300000,
  egress_bitrate: 7118000000,
  ingress_bytes_total: 987634212,
  egress_bytes_total: 482376543210,
  outbound_queue_bytes: 1843200,
  dropped_video_frames: 14,
  dropped_audio_frames: 0,
  slow_viewer_evictions: 2,
  process_memory_bytes: 1288490188,
  worker_cpu_usage: 6720,
  system_cpu_usage_milli_percent: 62800,
  "system_cpu_core_usage_milli_percent:0": 71000,
  "system_cpu_core_usage_milli_percent:1": 58400,
  "system_cpu_core_usage_milli_percent:2": 82200,
  "system_cpu_core_usage_milli_percent:3": 49700,
  "system_cpu_core_usage_milli_percent:4": 66300,
  "system_cpu_core_usage_milli_percent:5": 54100,
  "system_cpu_core_usage_milli_percent:6": 73800,
  "system_cpu_core_usage_milli_percent:7": 47000,
  cpu_cores_available: 8,
  gop_cache_bytes: 38482944
};

function parsePrometheus(text: string): Record<string, number> {
  const metrics: Record<string, number> = {};
  for (const line of text.split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const match = line.match(/^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{([^}]*)\})?\s+(-?(?:\d+\.?\d*|\.\d+))/);
    if (!match) continue;
    const value = Number(match[3]);
    if (!Number.isFinite(value)) continue;
    if (match[1] === "system_cpu_core_usage_milli_percent" && match[2]) {
      const core = match[2].match(/(?:^|,)core="(\d+)"(?:,|$)/);
      if (core) {
        metrics[`${match[1]}:${core[1]}`] = value;
        continue;
      }
    }
    metrics[match[1]] = (metrics[match[1]] ?? 0) + value;
  }
  return metrics;
}

export class ControlClient {
  private edgeStatsCache?: { fetchedAt: number; value: EdgeDeliveryStats | null };

  constructor(
    public readonly demo = false,
    private baseUrl = "/api"
  ) {}

  private async request<T>(path: string, init: RequestInit = {}): Promise<T> {
    const response = await fetch(`${this.baseUrl}${path}`, {
      ...init,
      headers: {
        Accept: "application/json",
        ...(init.body ? { "Content-Type": "application/json" } : {}),
        ...init.headers
      }
    });
    if (!response.ok) {
      let message = `Request failed (${response.status})`;
      try {
        const body = (await response.json()) as { message?: string };
        if (body.message) message = body.message;
      } catch {
        // Keep the safe status-based message for a non-JSON proxy error.
      }
      throw new ApiError(message, response.status, response.headers.get("X-Request-Id") ?? undefined);
    }
    const text = await response.text();
    return (text ? JSON.parse(text) : {}) as T;
  }

  // Best-effort: served as a static file outside /api, and simply absent
  // until viewer-estimator.service has completed its first write, so a
  // missing/unreachable file falls back silently to the API's own count.
  private async fetchEdgeStats(): Promise<EdgeDeliveryStats | null> {
    if (this.demo) return null;
    const now = Date.now();
    if (this.edgeStatsCache && now - this.edgeStatsCache.fetchedAt < 1000) {
      return this.edgeStatsCache.value;
    }
    try {
      const response = await fetch("/internal/viewer_estimate.json", {
        cache: "no-store",
        headers: { Accept: "application/json" }
      });
      if (!response.ok) throw new Error("edge stats unavailable");
      const body = (await response.json()) as Partial<EdgeDeliveryStats>;
      const maxAgeSeconds = Math.max(30, (body.window_seconds ?? 20) + 10);
      if (!Number.isFinite(body.generated_at) || Math.abs(Date.now() / 1000 - (body.generated_at ?? 0)) > maxAgeSeconds) {
        throw new Error("edge stats stale");
      }
      const cleanMap = (value: unknown): Record<string, number> => {
        if (!value || typeof value !== "object" || Array.isArray(value)) return {};
        return Object.fromEntries(Object.entries(value).filter((entry): entry is [string, number] =>
          typeof entry[1] === "number" && Number.isFinite(entry[1]) && entry[1] >= 0
        ));
      };
      const stats: EdgeDeliveryStats = {
        generated_at: body.generated_at as number,
        window_seconds: Number(body.window_seconds) || 20,
        viewers: cleanMap(body.viewers),
        bytes_total: cleanMap(body.bytes_total),
        bitrate_bps: cleanMap(body.bitrate_bps),
        totals: {
          viewers: 0,
          bytes_total: 0,
          bitrate_bps: 0
        }
      };
      const totals = body.totals;
      stats.totals = {
        viewers: typeof totals?.viewers === "number" && Number.isFinite(totals.viewers) && totals.viewers >= 0
          ? totals.viewers
          : Object.values(stats.viewers).reduce((sum, value) => sum + value, 0),
        bytes_total: typeof totals?.bytes_total === "number" && Number.isFinite(totals.bytes_total) && totals.bytes_total >= 0
          ? totals.bytes_total
          : Object.values(stats.bytes_total).reduce((sum, value) => sum + value, 0),
        bitrate_bps: typeof totals?.bitrate_bps === "number" && Number.isFinite(totals.bitrate_bps) && totals.bitrate_bps >= 0
          ? totals.bitrate_bps
          : Object.values(stats.bitrate_bps).reduce((sum, value) => sum + value, 0)
      };
      this.edgeStatsCache = { fetchedAt: now, value: stats };
      return stats;
    } catch {
      this.edgeStatsCache = { fetchedAt: now, value: null };
      return null;
    }
  }

  async snapshot(): Promise<Snapshot> {
    if (this.demo) {
      demoStreams = demoStreams.map((stream) =>
        stream.is_live
          ? { ...stream, egress_bytes_total: (stream.egress_bytes_total ?? 0) + (stream.viewer_count ?? 0) * 250000 }
          : stream
      );
      return {
        applications: [...demoApps],
        streams: demoStreams.map((stream) => ({ ...stream })),
        metrics: { ...demoMetrics },
        health: "online"
      };
    }

    const appsResponse = await this.request<ListResponse<Application>>("/v1/applications?limit=200");
    const streams = (
      await Promise.all(
        appsResponse.items.map((app) =>
          this.request<ListResponse<Stream>>(`/v1/streams?application=${encodeURIComponent(app.name)}`)
        )
      )
    ).flatMap((response) => response.items);

    const edgeStats = await this.fetchEdgeStats();
    const withStatus = await Promise.all(
      streams.map(async (stream) => {
        try {
          const status = await this.request<{
            is_live: boolean;
            viewer_count: number;
            rtmp_viewer_count?: number;
            hls_viewer_count?: number;
            hls_viewers_measured?: boolean;
            egress_bytes_total?: number;
            rtmp_egress_bytes_total?: number;
            hls_egress_bytes_total?: number;
          }>(
            `/v1/streams/${encodeURIComponent(`${stream.application}:${stream.name}`)}/status`
          );
          const merged = { ...stream, ...status };
          // A server that reads the cache-edge accounting itself already
          // reports the true total in viewer_count, with the RTMP/HLS split
          // alongside it. Adding this client's own edge reading on top would
          // count the whole HLS audience twice.
          if (status.hls_viewers_measured !== undefined) {
            // The server already split viewers and bytes by delivery path;
            // only the live bitrate still has no server-side field.
            return {
              ...merged,
              hls_egress_bitrate_bps: edgeStats
                ? exactEdgeValue(edgeStats.bitrate_bps, stream.application, stream.name)
                : merged.hls_egress_bitrate_bps
            };
          }
          const rtmpBytes = merged.egress_bytes_total ?? 0;
          const hlsViewers = edgeStats ? exactEdgeValue(edgeStats.viewers, stream.application, stream.name) : 0;
          const hlsBytes = edgeStats ? exactEdgeValue(edgeStats.bytes_total, stream.application, stream.name) : 0;
          return {
            ...merged,
            viewer_count: (merged.viewer_count ?? 0) + hlsViewers,
            egress_bytes_total: rtmpBytes + hlsBytes,
            rtmp_egress_bytes_total: rtmpBytes,
            ...(edgeStats ? {
              hls_viewer_count: hlsViewers,
              hls_egress_bytes_total: hlsBytes,
              hls_egress_bitrate_bps: exactEdgeValue(edgeStats.bitrate_bps, stream.application, stream.name)
            } : {})
          };
        } catch {
          const hlsViewers = edgeStats ? exactEdgeValue(edgeStats.viewers, stream.application, stream.name) : 0;
          const hlsBytes = edgeStats ? exactEdgeValue(edgeStats.bytes_total, stream.application, stream.name) : 0;
          const rtmpBytes = stream.egress_bytes_total ?? 0;
          return {
            ...stream,
            viewer_count: (stream.viewer_count ?? 0) + hlsViewers,
            egress_bytes_total: rtmpBytes + hlsBytes,
            rtmp_egress_bytes_total: rtmpBytes,
            ...(edgeStats ? {
              hls_viewer_count: hlsViewers,
              hls_egress_bytes_total: hlsBytes,
              hls_egress_bitrate_bps: exactEdgeValue(edgeStats.bitrate_bps, stream.application, stream.name)
            } : {})
          };
        }
      })
    );

    let metrics: Record<string, number> = {};
    try {
      const response = await fetch(`${this.baseUrl}/metrics`, {
        headers: { Accept: "text/plain" }
      });
      if (response.ok) metrics = parsePrometheus(await response.text());
    } catch {
      // A metrics scrape failure degrades charts, not the control plane.
    }

    // A server that reads the edge accounting exports these itself. Only
    // synthesise them from the raw file when it did not (an older build, or
    // one configured without an edge path).
    const serverPublishesEdgeMetrics = metrics.edge_delivery_stats_available !== undefined;
    if (!serverPublishesEdgeMetrics) {
      if (edgeStats) {
        metrics.hls_active_viewers = edgeStats.totals.viewers;
        metrics.hls_egress_bytes_total = edgeStats.totals.bytes_total;
        metrics.hls_egress_bitrate = edgeStats.totals.bitrate_bps;
        metrics.edge_delivery_stats_available = 1;
      } else {
        metrics.edge_delivery_stats_available = 0;
      }
    }

    return { applications: appsResponse.items, streams: withStatus, metrics, health: "online" };
  }

  async createApplication(name: string): Promise<Application> {
    if (this.demo) {
      const app = { name, enabled: true };
      demoApps.push(app);
      return app;
    }
    return this.request<Application>("/v1/applications", {
      method: "POST",
      body: JSON.stringify({ name })
    });
  }

  async deleteApplication(application: string): Promise<void> {
    if (this.demo) {
      demoApps = demoApps.filter((item) => item.name !== application);
      demoStreams = demoStreams.filter((item) => item.application !== application);
      return;
    }
    await this.request<{ deleted: boolean }>(`/v1/applications/${encodeURIComponent(application)}`, {
      method: "DELETE"
    });
  }

  // Settings management has no meaningful demo-mode story (there is no real
  // config file to show/edit), so demo mode reports no fields rather than
  // fabricating ones a demo viewer could believe they changed something real.
  async listSettings(): Promise<SettingField[]> {
    if (this.demo) return [];
    return this.request<SettingField[]>("/v1/settings");
  }

  async updateSettings(values: Record<string, string>): Promise<SettingField[]> {
    if (this.demo) throw new ApiError("Settings cannot be changed in demo mode.", 403);
    return this.request<SettingField[]>("/v1/settings", {
      method: "POST",
      body: JSON.stringify(values)
    });
  }

  async listTemplates(): Promise<TemplateRecord[]> {
    if (this.demo) {
      return demoTemplates.map((item) => ({ ...item, presets: [...item.presets] }));
    }
    const response = await this.request<{ items: TemplateRecord[] }>("/v1/templates");
    return response.items;
  }

  async putTemplate(id: string, name: string, presets: unknown[]): Promise<TemplateRecord> {
    if (this.demo) {
      const record: TemplateRecord = { id, name, presets };
      demoTemplates = [...demoTemplates.filter((item) => item.id !== id), record];
      return record;
    }
    const response = await fetch(`${this.baseUrl}/v1/templates/${encodeURIComponent(id)}`, {
      method: "PUT",
      headers: {
        Accept: "application/json",
        "Content-Type": "application/json",
        "X-Template-Name": name
      },
      body: JSON.stringify(presets)
    });
    if (!response.ok) {
      let message = `Request failed (${response.status})`;
      try {
        const body = (await response.json()) as { message?: string };
        if (body.message) message = body.message;
      } catch {
        // Keep status-based fallback.
      }
      throw new ApiError(message, response.status, response.headers.get("X-Request-Id") ?? undefined);
    }
    return response.json() as Promise<TemplateRecord>;
  }

  async deleteTemplate(id: string): Promise<void> {
    if (this.demo) {
      demoTemplates = demoTemplates.filter((item) => item.id !== id);
      return;
    }
    await this.request<{ deleted: boolean }>(`/v1/templates/${encodeURIComponent(id)}`, { method: "DELETE" });
  }

  async createStream(application: string, name: string, recording: boolean): Promise<StreamSetup> {
    if (this.demo) {
      const rtmpUrl = demoRtmpUrl(application, name);
      const stream = { application, name, enabled: true, recording_enabled: recording, rtmp_url: rtmpUrl, hls_path: `/hls/${application}/${name}/index.m3u8`, is_live: false, viewer_count: 0 };
      demoStreams = [...demoStreams, stream];
      return stream;
    }
    return this.request<StreamSetup>("/v1/streams", {
      method: "POST",
      body: JSON.stringify({ application, name, recording_enabled: recording })
    });
  }

  async patchStream(stream: Stream, fields: Partial<Pick<Stream, "enabled" | "recording_enabled">>): Promise<Stream> {
    if (this.demo) {
      demoStreams = demoStreams.map((item) =>
        item.application === stream.application && item.name === stream.name ? { ...item, ...fields } : item
      );
      return { ...stream, ...fields };
    }
    return this.request<Stream>(`/v1/streams/${encodeURIComponent(`${stream.application}:${stream.name}`)}`, {
      method: "PATCH",
      body: JSON.stringify(fields)
    });
  }

  async deleteStream(stream: Stream): Promise<void> {
    if (this.demo) {
      demoStreams = demoStreams.filter(
        (item) => item.application !== stream.application || item.name !== stream.name
      );
      return;
    }
    await this.request<{ deleted: boolean }>(
      `/v1/streams/${encodeURIComponent(`${stream.application}:${stream.name}`)}`,
      { method: "DELETE" }
    );
  }

  async disconnect(stream: Stream, target: "publisher" | "viewers"): Promise<void> {
    if (this.demo) {
      demoStreams = demoStreams.map((item) =>
        item.application === stream.application && item.name === stream.name
          ? { ...item, ...(target === "publisher" ? { is_live: false } : { viewer_count: 0 }) }
          : item
      );
      return;
    }
    await this.request(
      `/v1/streams/${encodeURIComponent(`${stream.application}:${stream.name}`)}/disconnect-${target}`,
      { method: "POST" }
    );
  }

  async listSourceTranscodes(application: string): Promise<SourceTranscodeJob[]> {
    if (this.demo) {
      return demoSourceJobs.filter((job) => job.application === application).map((job) => ({ ...job, outputs: [...job.outputs] }));
    }
    const [response, edgeStats] = await Promise.all([
      this.request<{ items: SourceTranscodeJob[] }>(
        `/v1/transcoding/source-jobs?application=${encodeURIComponent(application)}`
      ),
      this.fetchEdgeStats()
    ]);
    return response.items.map((job) => {
      // The server reads the same edge accounting now, and reports it per
      // rendition as well as per job. Re-deriving it here would only replace
      // correct numbers with a client-side approximation of them.
      if (job.delivery_stats_available) return job;
      if (!edgeStats) return job;
      const viewers = sourceEdgeValue(edgeStats.viewers, job.application, job.name, job.outputs);
      const delivered = sourceEdgeValue(edgeStats.bytes_total, job.application, job.name, job.outputs);
      const rate = sourceEdgeValue(edgeStats.bitrate_bps, job.application, job.name, job.outputs);
      return {
        ...job,
        viewer_count: viewers.matched ? viewers.value : job.viewer_count,
        bytes_total: delivered.matched ? delivered.value : job.bytes_total,
        hls_egress_bitrate_bps: rate.matched ? rate.value : 0,
        delivery_stats_available: true
      };
    });
  }

  async createSourceTranscode(
    application: string,
    name: string,
    sourceUrl: string,
    templateName: string,
    rules: string,
    outputs: TranscodingOutput[],
    autoRestart: boolean = true,
    restartDelaySeconds: number = 5
  ): Promise<SourceTranscodeJob> {
    if (this.demo) {
      const job: SourceTranscodeJob = {
        id: `${application}:${name}`,
        application,
        name,
        source_url: sourceUrl,
        template_name: templateName,
        master_hls_path: `/hls/${application}/${name}/master.m3u8`,
        outputs,
        status: "running",
        enabled: true,
        auto_restart: autoRestart,
        restart_delay_seconds: restartDelaySeconds
      };
      demoSourceJobs = [...demoSourceJobs.filter((item) => item.id !== job.id), job];
      return job;
    }
    const response = await fetch(`${this.baseUrl}/v1/transcoding/source-jobs`, {
      method: "POST",
      headers: {
        Accept: "application/json",
        "Content-Type": "text/plain; charset=utf-8",
        "X-Application": application,
        "X-Output-Name": name,
        "X-Source-Url": sourceUrl,
        "X-Template-Name": templateName,
        "X-Auto-Restart": autoRestart ? "true" : "false",
        "X-Restart-Delay-Seconds": String(restartDelaySeconds)
      },
      body: rules
    });
    if (!response.ok) {
      let message = `Request failed (${response.status})`;
      try {
        const body = (await response.json()) as { message?: string };
        if (body.message) message = body.message;
      } catch {
        // Keep status-based fallback.
      }
      throw new ApiError(message, response.status, response.headers.get("X-Request-Id") ?? undefined);
    }
    return response.json() as Promise<SourceTranscodeJob>;
  }

  async patchSourceTranscode(job: SourceTranscodeJob, enabled: boolean): Promise<SourceTranscodeJob> {
    if (this.demo) {
      const status = enabled ? "running" : "disabled";
      demoSourceJobs = demoSourceJobs.map((item) => (item.id === job.id ? { ...item, enabled, status } : item));
      return { ...job, enabled, status };
    }
    return this.request<SourceTranscodeJob>(`/v1/transcoding/source-jobs/${encodeURIComponent(job.id)}`, {
      method: "PATCH",
      body: JSON.stringify({ enabled })
    });
  }

  async restartSourceTranscode(job: SourceTranscodeJob): Promise<SourceTranscodeJob> {
    if (this.demo) {
      demoSourceJobs = demoSourceJobs.map((item) => (item.id === job.id ? { ...item, status: "running" } : item));
      return { ...job, status: "running" };
    }
    return this.request<SourceTranscodeJob>(
      `/v1/transcoding/source-jobs/${encodeURIComponent(job.id)}/restart`,
      { method: "POST" }
    );
  }

  async removeSourceTranscode(job: SourceTranscodeJob): Promise<void> {
    if (this.demo) {
      demoSourceJobs = demoSourceJobs.filter((item) => item.id !== job.id);
      return;
    }
    await this.request<{ deleted: boolean }>(
      `/v1/transcoding/source-jobs/${encodeURIComponent(job.id)}`,
      { method: "DELETE" }
    );
  }
}
