export type Application = {
  name: string;
  enabled: boolean;
};

export type Stream = {
  application: string;
  name: string;
  enabled: boolean;
  recording_enabled: boolean;
  playback_url?: string;
  is_live?: boolean;
  viewer_count?: number;
};

export type StreamSetup = {
  stream?: Stream;
  publish_name: string;
  publish_url?: string;
  playback_url?: string;
};

export type Snapshot = {
  applications: Application[];
  streams: Stream[];
  metrics: Record<string, number>;
  health: "online" | "degraded" | "offline";
};

type ListResponse<T> = { items: T[]; total: number };

export class ApiError extends Error {
  constructor(
    message: string,
    public status: number,
    public requestId?: string
  ) {
    super(message);
  }
}

const demoApps: Application[] = [
  { name: "live", enabled: true },
  { name: "events", enabled: true },
  { name: "backup", enabled: false }
];

const demoPlayback = (application: string, name: string) => `rtmp://stream.example.com:1935/${application}/${name}`;

let demoStreams: Stream[] = [
  { application: "live", name: "main-stage", enabled: true, recording_enabled: true, playback_url: demoPlayback("live", "main-stage"), is_live: true, viewer_count: 3842 },
  { application: "live", name: "sports-east", enabled: true, recording_enabled: false, playback_url: demoPlayback("live", "sports-east"), is_live: true, viewer_count: 1947 },
  { application: "events", name: "conference-a", enabled: true, recording_enabled: true, playback_url: demoPlayback("events", "conference-a"), is_live: true, viewer_count: 826 },
  { application: "events", name: "studio-feed", enabled: true, recording_enabled: false, playback_url: demoPlayback("events", "studio-feed"), is_live: false, viewer_count: 0 },
  { application: "backup", name: "disaster-recovery", enabled: false, recording_enabled: false, playback_url: demoPlayback("backup", "disaster-recovery"), is_live: false, viewer_count: 0 }
];

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
  gop_cache_bytes: 38482944
};

function parsePrometheus(text: string): Record<string, number> {
  const metrics: Record<string, number> = {};
  for (const line of text.split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const match = line.match(/^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{[^}]*\})?\s+(-?(?:\d+\.?\d*|\.\d+))/);
    if (!match) continue;
    const value = Number(match[2]);
    if (!Number.isFinite(value)) continue;
    metrics[match[1]] = (metrics[match[1]] ?? 0) + value;
  }
  return metrics;
}

export class ControlClient {
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

  async snapshot(): Promise<Snapshot> {
    if (this.demo) {
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

    const withStatus = await Promise.all(
      streams.map(async (stream) => {
        try {
          const status = await this.request<{ is_live: boolean; viewer_count: number }>(
            `/v1/streams/${encodeURIComponent(`${stream.application}:${stream.name}`)}/status`
          );
          return { ...stream, ...status };
        } catch {
          return { ...stream };
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

  async createStream(application: string, name: string, recording: boolean): Promise<StreamSetup> {
    if (this.demo) {
      const stream = { application, name, enabled: true, recording_enabled: recording, playback_url: demoPlayback(application, name), is_live: false, viewer_count: 0 };
      demoStreams = [...demoStreams, stream];
      return {
        stream,
        publish_name: name,
        publish_url: `rtmp://stream.example.com:1935/${application}`,
        playback_url: `rtmp://stream.example.com:1935/${application}/${name}`
      };
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
}
