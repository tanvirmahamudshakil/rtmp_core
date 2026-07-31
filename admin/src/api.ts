export type Application = {
  name: string;
  enabled: boolean;
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
};

export type StreamSetup = Stream;

export type TranscodingOutput = {
  name: string;
  stream: string;
  video_codec: string;
  video_bitrate: number;
  width: number;
  height: number;
};

export type TranscodingAssignment = {
  application: string;
  source_stream: string;
  template_name: string;
  master_hls_path: string;
  outputs: TranscodingOutput[];
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
let demoAssignments: TranscodingAssignment[] = [];
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
  cpu_cores_available: 8,
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

  async listTranscodingAssignments(application: string): Promise<TranscodingAssignment[]> {
    if (this.demo) {
      return demoAssignments.filter((item) => item.application === application).map((item) => ({ ...item, outputs: [...item.outputs] }));
    }
    const response = await this.request<{ items: TranscodingAssignment[] }>(
      `/v1/transcoding/assignments?application=${encodeURIComponent(application)}`
    );
    return response.items;
  }

  async assignTranscodingTemplate(
    stream: Stream,
    templateName: string,
    rules: string,
    outputs: TranscodingOutput[]
  ): Promise<TranscodingAssignment> {
    if (this.demo) {
      const assignment: TranscodingAssignment = {
        application: stream.application,
        source_stream: stream.name,
        template_name: templateName,
        master_hls_path: `/hls/${stream.application}/${stream.name}/master.m3u8`,
        outputs
      };
      demoAssignments = [
        ...demoAssignments.filter((item) => item.application !== stream.application || item.source_stream !== stream.name),
        assignment
      ];
      return assignment;
    }
    const response = await fetch(
      `${this.baseUrl}/v1/transcoding/assignments/${encodeURIComponent(`${stream.application}:${stream.name}`)}`,
      {
        method: "PUT",
        headers: {
          Accept: "application/json",
          "Content-Type": "text/plain; charset=utf-8",
          "X-Template-Name": templateName
        },
        body: rules
      }
    );
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
    return response.json() as Promise<TranscodingAssignment>;
  }

  async removeTranscodingAssignment(stream: Stream): Promise<void> {
    if (this.demo) {
      demoAssignments = demoAssignments.filter(
        (item) => item.application !== stream.application || item.source_stream !== stream.name
      );
      return;
    }
    await this.request<{ deleted: boolean }>(
      `/v1/transcoding/assignments/${encodeURIComponent(`${stream.application}:${stream.name}`)}`,
      { method: "DELETE" }
    );
  }

  async listSourceTranscodes(application: string): Promise<SourceTranscodeJob[]> {
    if (this.demo) {
      return demoSourceJobs.filter((job) => job.application === application).map((job) => ({ ...job, outputs: [...job.outputs] }));
    }
    const response = await this.request<{ items: SourceTranscodeJob[] }>(
      `/v1/transcoding/source-jobs?application=${encodeURIComponent(application)}`
    );
    return response.items;
  }

  async createSourceTranscode(
    application: string,
    name: string,
    sourceUrl: string,
    templateName: string,
    rules: string,
    outputs: TranscodingOutput[]
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
        enabled: true
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
        "X-Template-Name": templateName
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
