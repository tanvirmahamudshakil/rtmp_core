import {
  Activity,
  AppWindow,
  ArrowDownToLine,
  ArrowLeft,
  ArrowUpRight,
  Check,
  ChevronRight,
  CircleGauge,
  CloudOff,
  Copy,
  Cpu,
  Database,
  FileVideo,
  HardDrive,
  House,
  Layers3,
  Menu,
  MoreHorizontal,
  Network,
  Plus,
  Radio,
  RadioTower,
  RefreshCw,
  Router,
  Server,
  Settings2,
  ShieldCheck,
  Signal,
  SlidersHorizontal,
  SquareActivity,
  Trash2,
  Unplug,
  Users,
  Video,
  Workflow,
  X,
  Zap
} from "lucide-react";
import { FormEvent, ReactNode, useCallback, useEffect, useRef, useState } from "react";
import { Application, ControlClient, Snapshot, SourceTranscodeJob, Stream, TemplateRecord, TranscodingAssignment, TranscodingOutput } from "./api";

type Page = "home" | "applications" | "transcode" | "server";
type ApplicationTab = "playback" | "transcoding" | "source";
type VideoCodec = "H.263" | "H.264" | "H.265" | "VP8" | "VP9" | "Passthrough" | "Disabled";
type EncodingImplementation = "Beamr" | "QuickSync" | "NVENC" | "Default";
type VideoProfile = "baseline" | "main" | "high";
type FitMode = "match-source" | "fit-width" | "fit-height" | "crop" | "stretch" | "letterbox";
type AudioCodec = "AAC" | "Vorbis" | "Opus" | "Passthrough" | "Disabled";
type EncodingPreset = {
  id: string;
  name: string;
  outgoingStreamName: string;
  description: string;
  videoCodec: VideoCodec;
  videoBitrate: number;
  implementation: EncodingImplementation;
  profile: VideoProfile;
  keyFrameMode: "source" | "interval";
  keyFrameInterval: number | null;
  frameWidth: number | null;
  frameHeight: number | null;
  fitMode: FitMode;
  audioCodec: AudioCodec;
  audioBitrate: number;
  gpuMode: "first" | "specific";
  gpuId: number | null;
  enabled: boolean;
};
type TranscodingTemplate = {
  id: string;
  name: string;
  presets: EncodingPreset[];
};
type Notice = { type: "success" | "error"; message: string } | null;

const EMPTY_SNAPSHOT: Snapshot = { applications: [], streams: [], metrics: {}, health: "offline" };
const absoluteUrl = (path: string) => new URL(path, window.location.origin).toString();
const pageTitles: Record<Page, { title: string; subtitle: string }> = {
  home: { title: "Home", subtitle: "Live delivery overview and network activity" },
  applications: { title: "Application", subtitle: "Manage applications, streams and playback links" },
  transcode: { title: "Transcode", subtitle: "Your transcoding workspace" },
  server: { title: "Server", subtitle: "Origin health, resources and delivery capacity" }
};
const applicationTabs: { id: ApplicationTab; label: string }[] = [
  { id: "playback", label: "Playback URLs" },
  { id: "transcoding", label: "Transcoding" },
  { id: "source", label: "Source Transcode" }
];
const videoCodecs: VideoCodec[] = ["H.263", "H.264", "H.265", "VP8", "VP9", "Passthrough", "Disabled"];
const encodingImplementations: EncodingImplementation[] = ["Beamr", "QuickSync", "NVENC", "Default"];
const videoProfiles: VideoProfile[] = ["baseline", "main", "high"];
const allFitModes: FitMode[] = ["match-source", "fit-width", "fit-height", "crop", "stretch", "letterbox"];
const outputFitModes: FitMode[] = ["letterbox", "crop", "stretch"];
const fitModeLabels: Record<FitMode, string> = {
  "match-source": "Match source",
  "fit-width": "Fit width",
  "fit-height": "Fit height",
  crop: "Crop to fill",
  stretch: "Stretch",
  letterbox: "Letterbox"
};
const frameResolutions = [
  { id: "source", label: "Source resolution", width: null, height: null },
  { id: "256x144", label: "144p · 256 × 144", width: 256, height: 144 },
  { id: "426x240", label: "240p · 426 × 240", width: 426, height: 240 },
  { id: "640x360", label: "360p · 640 × 360", width: 640, height: 360 },
  { id: "854x480", label: "480p · 854 × 480", width: 854, height: 480 },
  { id: "960x540", label: "540p · 960 × 540", width: 960, height: 540 },
  { id: "1280x720", label: "720p HD · 1280 × 720", width: 1280, height: 720 },
  { id: "1920x1080", label: "1080p Full HD · 1920 × 1080", width: 1920, height: 1080 },
  { id: "2560x1440", label: "1440p QHD · 2560 × 1440", width: 2560, height: 1440 },
  { id: "3840x2160", label: "2160p 4K · 3840 × 2160", width: 3840, height: 2160 },
  { id: "custom", label: "Custom resolution…", width: null, height: null }
] as const;
type FrameResolutionId = (typeof frameResolutions)[number]["id"];
const audioCodecs: AudioCodec[] = ["AAC", "Vorbis", "Opus", "Passthrough", "Disabled"];
// What the in-process, FFmpeg-free native pipeline can actually do today: H.264
// (openh264) and H.265 (x265) video, AAC (libfdk-aac) audio, software backend
// only. Everything else is disabled in the pickers until its codec/backend is
// built, so an operator cannot select a rendition the origin cannot produce.
const builtVideoCodecs = new Set<VideoCodec>(["H.264", "H.265", "Passthrough", "Disabled"]);
const builtImplementations = new Set<EncodingImplementation>(["Default"]);
const builtAudioCodecs = new Set<AudioCodec>(["AAC", "Passthrough", "Disabled"]);
const newLocalId = () => globalThis.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
const fullAdaptiveLadderPresets = (): EncodingPreset[] => [
  ["2160p", 3840, 2160, 12000000, 256000, "high", "4K output"],
  ["1440p", 2560, 1440, 6000000, 192000, "high", "QHD output"],
  ["1080p", 1920, 1080, 3000000, 160000, "high", "Full HD output"],
  ["720p", 1280, 720, 1000000, 128000, "high", "HD output"],
  ["540p", 960, 540, 750000, 96000, "main", "qHD output"],
  ["480p", 854, 480, 500000, 96000, "main", "Mobile output"],
  ["360p", 640, 360, 490000, 64000, "main", "Low-bandwidth output"],
  ["240p", 426, 240, 250000, 48000, "baseline", "Very low-bandwidth output"],
  ["144p", 256, 144, 120000, 32000, "baseline", "Minimum-bandwidth output"]
].map(([name, frameWidth, frameHeight, videoBitrate, audioBitrate, profile, description]) => ({
  id: newLocalId(),
  name: String(name),
  outgoingStreamName: String(name),
  description: String(description),
  videoCodec: "H.264",
  videoBitrate: Number(videoBitrate),
  implementation: "Default",
  profile: profile as VideoProfile,
  keyFrameMode: "interval",
  keyFrameInterval: 60,
  frameWidth: Number(frameWidth),
  frameHeight: Number(frameHeight),
  fitMode: "letterbox",
  audioCodec: "AAC",
  audioBitrate: Number(audioBitrate),
  gpuMode: "first",
  gpuId: null,
  enabled: true
}));
const validFrameDimension = (value: unknown): value is number =>
  typeof value === "number" && Number.isInteger(value) && value >= 16 && value <= 8192 && value % 2 === 0;
// Normalizes a preset coming back from the server (or an older admin build)
// into the shape this UI relies on — same defaulting/repair logic that used
// to run against localStorage, now run against the SQLite-backed template API.
const normalizePreset = (preset: EncodingPreset): EncodingPreset => {
  let frameWidth = validFrameDimension(preset.frameWidth) ? preset.frameWidth : null;
  let frameHeight = validFrameDimension(preset.frameHeight) ? preset.frameHeight : null;
  let fitMode: FitMode = allFitModes.includes(preset.fitMode) ? preset.fitMode : "match-source";
  // Older control-panel builds allowed dimensions to be saved with
  // match-source, which made the transcoder ignore those dimensions.
  // Preserve valid requested sizes; reset malformed legacy values to
  // source resolution instead of submitting another invalid rule.
  if (fitMode === "match-source") {
    if (frameWidth && frameHeight) {
      fitMode = "letterbox";
    } else {
      frameWidth = null;
      frameHeight = null;
    }
  } else if (
    (fitMode === "fit-width" && !frameWidth) ||
    (fitMode === "fit-height" && !frameHeight) ||
    (["crop", "stretch", "letterbox"].includes(fitMode) && (!frameWidth || !frameHeight))
  ) {
    frameWidth = null;
    frameHeight = null;
    fitMode = "match-source";
  }
  return {
    ...preset,
    implementation: encodingImplementations.includes(preset.implementation) ? preset.implementation : "Default",
    profile: preset.profile ?? "high",
    keyFrameMode: preset.keyFrameMode ?? "source",
    keyFrameInterval: preset.keyFrameInterval ?? null,
    frameWidth,
    frameHeight,
    fitMode,
    audioCodec: preset.audioCodec ?? "AAC",
    audioBitrate: preset.audioBitrate ?? 128000,
    enabled: preset.enabled ?? true
  };
};
const templatesFromRecords = (records: TemplateRecord[]): TranscodingTemplate[] =>
  records.map((record) => ({
    id: record.id,
    name: record.name,
    presets: Array.isArray(record.presets) ? (record.presets as EncodingPreset[]).map(normalizePreset) : []
  }));

const safeRuleName = (value: string, fallback: string) => {
  const safe = value.trim().replace(/[^A-Za-z0-9._-]+/g, "-").replace(/^-+|-+$/g, "");
  return (safe || fallback).slice(0, 96);
};
const backendRuleValue: Record<EncodingImplementation, string> = {
  Default: "default",
  NVENC: "nvenc",
  QuickSync: "quicksync",
  Beamr: "beamr"
};
const videoRuleValue: Record<VideoCodec, string> = {
  "H.263": "h263",
  "H.264": "h264",
  "H.265": "h265",
  VP8: "vp8",
  VP9: "vp9",
  Passthrough: "passthrough",
  Disabled: "disabled"
};
const audioRuleValue: Record<AudioCodec, string> = {
  AAC: "aac",
  Vorbis: "vorbis",
  Opus: "opus",
  Passthrough: "passthrough",
  Disabled: "disabled"
};
const buildTemplateRules = (stream: Stream, template: TranscodingTemplate) => {
  const outputs: TranscodingOutput[] = [];
  const activePresets = template.presets.filter((preset) => preset.enabled);
  const lines = activePresets.map((preset, index) => {
    const outputSuffix = safeRuleName(preset.outgoingStreamName, `output-${index + 1}`);
    const outputStream = safeRuleName(`${stream.name}_${outputSuffix}`, `${stream.name}_output-${index + 1}`);
    outputs.push({
      name: preset.name,
      stream: outputStream,
      video_codec: videoRuleValue[preset.videoCodec],
      video_bitrate: preset.videoBitrate,
      width: preset.frameWidth ?? 0,
      height: preset.frameHeight ?? 0
    });
    const description = preset.description.replace(/[|\r\n]+/g, " ").slice(0, 180);
    return [
      `${stream.application}/${stream.name}`,
      safeRuleName(preset.name, `preset-${index + 1}`),
      outputStream,
      backendRuleValue[preset.implementation],
      videoRuleValue[preset.videoCodec],
      preset.videoBitrate || 2500000,
      preset.profile,
      preset.keyFrameMode === "source" ? "source" : preset.keyFrameInterval ?? 60,
      preset.frameWidth ?? "",
      preset.frameHeight ?? "",
      preset.fitMode,
      audioRuleValue[preset.audioCodec],
      preset.audioBitrate || 128000,
      preset.gpuMode === "specific" ? preset.gpuId ?? 0 : "first",
      description
    ].join("|");
  });
  return { rules: lines.join("\n"), outputs };
};

// Rules for a source-driven job. The origin overrides the input with the source
// URL, so the pseudo-stream only shapes the output names and rendition ladder.
const buildSourceRules = (application: string, outputName: string, template: TranscodingTemplate) => {
  const pseudo = {
    application,
    name: outputName,
    enabled: true,
    recording_enabled: false,
    rtmp_url: "",
    hls_path: ""
  } as Stream;
  return buildTemplateRules(pseudo, template);
};

const metric = (snapshot: Snapshot, name: string) => snapshot.metrics[name] ?? 0;
const compact = (value: number) =>
  new Intl.NumberFormat("en", { notation: "compact", maximumFractionDigits: 1 }).format(value);
const bytes = (value: number) => {
  if (!value) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB"];
  const index = Math.min(Math.floor(Math.log(value) / Math.log(1024)), units.length - 1);
  return `${(value / 1024 ** index).toFixed(index > 1 ? 1 : 0)} ${units[index]}`;
};
const bitrate = (bits: number) => {
  if (bits >= 1e9) return `${(bits / 1e9).toFixed(2)} Gbps`;
  if (bits >= 1e6) return `${(bits / 1e6).toFixed(1)} Mbps`;
  return `${(bits / 1e3).toFixed(0)} Kbps`;
};

// Best-effort clipboard copy that also works on plain HTTP, where the async
// Clipboard API is blocked; falls back to a hidden textarea + execCommand.
async function copyText(value: string) {
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(value);
      return;
    }
  } catch {
    // fall through to the legacy path
  }
  const fallback = document.createElement("textarea");
  fallback.value = value;
  fallback.setAttribute("readonly", "");
  fallback.style.position = "fixed";
  fallback.style.opacity = "0";
  document.body.appendChild(fallback);
  fallback.select();
  document.execCommand("copy");
  fallback.remove();
}

function IconButton({
  label,
  children,
  onClick,
  danger = false
}: {
  label: string;
  children: ReactNode;
  onClick?: () => void;
  danger?: boolean;
}) {
  return (
    <button className={`icon-button ${danger ? "danger" : ""}`} title={label} aria-label={label} onClick={onClick}>
      {children}
    </button>
  );
}

function StatusPill({ live, label }: { live?: boolean; label?: string }) {
  return (
    <span className={`status-pill ${live ? "live" : "idle"}`}>
      <span className="status-dot" />
      {label ?? (live ? "Live" : "Idle")}
    </span>
  );
}

function Modal({
  title,
  description,
  children,
  onClose,
  wide = false
}: {
  title: string;
  description?: string;
  children: ReactNode;
  onClose: () => void;
  wide?: boolean;
}) {
  return (
    <div className="modal-backdrop" role="presentation" onMouseDown={onClose}>
      <section
        className={`modal ${wide ? "wide" : ""}`}
        role="dialog"
        aria-modal="true"
        aria-labelledby="modal-title"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="modal-heading">
          <div>
            <span className="eyebrow">STREAM CONTROL</span>
            <h2 id="modal-title">{title}</h2>
            {description && <p>{description}</p>}
          </div>
          <IconButton label="Close dialog" onClick={onClose}>
            <X size={18} />
          </IconButton>
        </div>
        {children}
      </section>
    </div>
  );
}

function Sidebar({
  page,
  setPage,
  open,
  close
}: {
  page: Page;
  setPage: (page: Page) => void;
  open: boolean;
  close: () => void;
}) {
  const nav: { page: Page; label: string; icon: ReactNode }[] = [
    { page: "home", label: "Home", icon: <House size={18} /> },
    { page: "applications", label: "Application", icon: <Layers3 size={18} /> },
    { page: "transcode", label: "Transcode", icon: <Workflow size={18} /> },
    { page: "server", label: "Server", icon: <Server size={18} /> }
  ];
  return (
    <>
      {open && <button className="mobile-scrim" aria-label="Close navigation" onClick={close} />}
      <aside className={`sidebar ${open ? "open" : ""}`}>
        <div className="brand-lockup">
          <div className="brand-mark"><RadioTower size={21} /></div>
          <div><strong>StreamForge</strong><span>CONTROL PLANE</span></div>
        </div>
        <nav aria-label="Primary navigation">
          <span className="nav-label">WORKSPACE</span>
          {nav.map((item) => (
            <button
              key={item.page}
              className={page === item.page ? "active" : ""}
              onClick={() => {
                setPage(item.page);
                close();
              }}
            >
              {item.icon}<span>{item.label}</span>
              {page === item.page && <ChevronRight className="nav-arrow" size={15} />}
            </button>
          ))}
        </nav>
        <div className="sidebar-spacer" />
        <div className="edge-card">
          <div className="edge-icon"><RadioTower size={18} /></div>
          <div><strong>C++ media origin</strong><span>RTMP ingest · HLS delivery</span></div>
          <StatusPill live label="Ready" />
        </div>
        <div className="sidebar-version">StreamForge v1.0 · C++23</div>
      </aside>
    </>
  );
}

function Sparkline({ data, color = "#ff9f43" }: { data: number[]; color?: string }) {
  const values = data.length > 1 ? data : [0, 0];
  const max = Math.max(...values, 1);
  const min = Math.min(...values);
  const span = max - min || 1;
  const points = values
    .map((value, index) => `${(index / (values.length - 1)) * 100},${38 - ((value - min) / span) * 34}`)
    .join(" ");
  return (
    <svg className="sparkline" viewBox="0 0 100 40" preserveAspectRatio="none" aria-hidden="true">
      <defs>
        <linearGradient id={`fade-${color.replace("#", "")}`} x1="0" x2="0" y1="0" y2="1">
          <stop offset="0%" stopColor={color} stopOpacity=".28" />
          <stop offset="100%" stopColor={color} stopOpacity="0" />
        </linearGradient>
      </defs>
      <polygon points={`0,40 ${points} 100,40`} fill={`url(#fade-${color.replace("#", "")})`} />
      <polyline points={points} fill="none" stroke={color} strokeWidth="1.7" vectorEffect="non-scaling-stroke" />
    </svg>
  );
}

function MetricCard({
  icon,
  label,
  value,
  helper,
  tone = "amber",
  history
}: {
  icon: ReactNode;
  label: string;
  value: string;
  helper: string;
  tone?: "amber" | "teal" | "blue" | "purple";
  history?: number[];
}) {
  const colors = { amber: "#ff9f43", teal: "#2dd4bf", blue: "#5da9ff", purple: "#a78bfa" };
  return (
    <article className="metric-card">
      <div className={`metric-icon ${tone}`}>{icon}</div>
      <div className="metric-copy"><span>{label}</span><strong>{value}</strong><small>{helper}</small></div>
      {history && <Sparkline data={history} color={colors[tone]} />}
    </article>
  );
}

function Overview({
  snapshot,
  history,
  bandwidth,
  streamBandwidth,
  sourceJobs,
  sourceBandwidth,
  onNavigate,
  onStreamAction
}: {
  snapshot: Snapshot;
  history: number[];
  bandwidth: number;
  streamBandwidth: Record<string, number>;
  sourceJobs: SourceTranscodeJob[];
  sourceBandwidth: Record<string, number>;
  onNavigate: (page: Page) => void;
  onStreamAction: (stream: Stream, action: string) => void;
}) {
  const viewers = metric(snapshot, "active_viewers");
  const publishers = metric(snapshot, "active_publishers");
  const egress = metric(snapshot, "egress_bitrate");
  const utilization = Math.min((egress / (bandwidth * 1e6)) * 100, 999);
  const links = buildLinkRows(snapshot.streams, sourceJobs, streamBandwidth, sourceBandwidth);
  return (
    <>
      <section className="metric-grid">
        <MetricCard icon={<Users size={20} />} label="Viewers now" value={compact(viewers)}
          helper={`${compact(metric(snapshot, "active_connections"))} active connections`} tone="amber" history={history} />
        <MetricCard icon={<RadioTower size={20} />} label="Live streams" value={compact(publishers)}
          helper={`${snapshot.streams.length} streams configured`} tone="teal" />
        <MetricCard icon={<ArrowUpRight size={20} />} label="Bandwidth out" value={bitrate(egress)}
          helper={`${utilization.toFixed(1)}% of available uplink`} tone="blue" />
        <MetricCard icon={<Cpu size={20} />} label="Server load"
          value={`${(metric(snapshot, "worker_cpu_usage") / 1000).toFixed(2)} / ${compact(metric(snapshot, "cpu_cores_available") || 1)} cores`}
          helper={`${bytes(metric(snapshot, "process_memory_bytes"))} resident memory`} tone="purple" />
      </section>

      <section className="overview-grid">
        <article className="panel traffic-panel">
          <div className="panel-heading">
            <div><span className="eyebrow">BANDWIDTH</span><h2>Network headroom</h2></div>
            <button className="subtle-button" onClick={() => onNavigate("server")}>
              Open planner <ChevronRight size={16} />
            </button>
          </div>
          <div className="throughput-layout">
            <div className="utilization-ring">
              <svg viewBox="0 0 100 100" aria-hidden="true">
                <circle className="ring-track" cx="50" cy="50" r="43" pathLength="100" />
                <circle
                  className="ring-value"
                  cx="50"
                  cy="50"
                  r="43"
                  pathLength="100"
                  strokeDasharray="100"
                  strokeDashoffset={100 - Math.min(utilization, 100)}
                />
              </svg>
              <div><strong>{utilization.toFixed(0)}%</strong><span>uplink used</span></div>
            </div>
            <div className="throughput-stats">
              <div><span><ArrowUpRight size={15} /> Egress now</span><strong>{bitrate(egress)}</strong></div>
              <div><span><ArrowDownToLine size={15} /> Ingress now</span><strong>{bitrate(metric(snapshot, "ingress_bitrate"))}</strong></div>
              <div><span><Network size={15} /> Declared link</span><strong>{bandwidth.toLocaleString()} Mbps</strong></div>
            </div>
          </div>
          <div className={`headroom-callout ${utilization > 80 ? "warning" : ""}`}>
            <Signal size={18} />
            <div>
              <strong>{utilization > 80 ? "Scale pressure detected" : "Delivery headroom is healthy"}</strong>
              <span>{Math.max(0, bandwidth - egress / 1e6).toFixed(0)} Mbps remains before line rate. Keep at least 10–20% for bursts and TCP recovery.</span>
            </div>
          </div>
        </article>

        <article className="panel health-panel">
          <div className="panel-heading">
            <div><span className="eyebrow">HEALTH</span><h2>Everything important</h2></div>
            <StatusPill live={snapshot.health === "online"} label={snapshot.health === "online" ? "Operational" : "Degraded"} />
          </div>
          <div className="health-list">
            <div><span className="health-icon good"><ShieldCheck size={18} /></span><p><strong>Connection limits</strong><small>Per-IP and per-stream safety limits</small></p><b>Active</b></div>
            <div><span className="health-icon good"><Database size={18} /></span><p><strong>SQLite control store</strong><small>Readiness check passed</small></p><b>Ready</b></div>
            <div><span className="health-icon good"><HardDrive size={18} /></span><p><strong>Outbound queue</strong><small>{bytes(metric(snapshot, "outbound_queue_bytes"))} pending</small></p><b>Stable</b></div>
            <div><span className={`health-icon ${metric(snapshot, "slow_viewer_evictions") > 10 ? "warn" : "good"}`}><Unplug size={18} /></span><p><strong>Slow viewers</strong><small>Automatic backpressure policy</small></p><b>{compact(metric(snapshot, "slow_viewer_evictions"))} evicted</b></div>
          </div>
        </article>
      </section>

      <section className="panel">
        <div className="panel-heading">
          <div><span className="eyebrow">LIVE NOW</span><h2>Top streams</h2></div>
          <button className="subtle-button" onClick={() => onNavigate("applications")}>View all streams <ChevronRight size={16} /></button>
        </div>
        <LinksTable rows={links} onAction={onStreamAction} />
      </section>
    </>
  );
}

// One row per deliverable link: a plain published stream or a source-transcode
// job's adaptive master. Merged into one list because operators mostly care
// about "what's live and what's it costing", not which admin tab it lives
// under (source-transcode jobs are the primary workload here — see the
// Transcode-from-URL feature).
type LinkRow = {
  key: string;
  kind: "stream" | "source";
  name: string;
  application: string;
  live: boolean;
  statusLabel: string;
  // Live viewer estimate. For a source-transcode link this is distinct
  // client IPs seen fetching its HLS renditions in the last ~20s (see
  // SourceTranscodeJob.viewer_count in api.ts for the caveats); for a plain
  // stream it's the RTMP fan-out's unique-IP subscriber count.
  viewers: number | undefined;
  // Live bitrate right now, refreshed every poll from the delta of a
  // cumulative byte counter (streamBandwidth / sourceBandwidth in App()).
  liveBitrateBps: number | undefined;
  // Total bytes delivered since the link went live (cumulative, not a rate)
  // — how much bandwidth this link has actually cost so far.
  totalBytes: number | undefined;
  renditionCount: number | undefined;
  copyUrl: string;
  copyLabel: string;
  stream?: Stream;
};

function buildLinkRows(
  streams: Stream[],
  sourceJobs: SourceTranscodeJob[],
  bandwidthByStream: Record<string, number>,
  bandwidthByJob: Record<string, number>
): LinkRow[] {
  const streamRows: LinkRow[] = streams.map((stream) => ({
    key: `stream:${stream.application}:${stream.name}`,
    kind: "stream",
    name: stream.name,
    application: stream.application,
    live: !!stream.is_live,
    statusLabel: stream.is_live ? "Live" : "Idle",
    viewers: stream.viewer_count,
    liveBitrateBps: stream.is_live ? bandwidthByStream[`${stream.application}:${stream.name}`] : undefined,
    totalBytes: stream.egress_bytes_total,
    renditionCount: undefined,
    copyUrl: absoluteUrl(stream.hls_path),
    copyLabel: "Copy HLS playback URL",
    stream
  }));
  const sourceRows: LinkRow[] = sourceJobs.map((job) => ({
    key: `source:${job.application}:${job.name}`,
    kind: "source",
    name: job.name,
    application: job.application,
    live: job.status === "running",
    statusLabel: job.enabled ? job.status : "Paused",
    viewers: job.status === "running" ? job.viewer_count : undefined,
    liveBitrateBps: job.status === "running" ? bandwidthByJob[`job:${job.application}:${job.name}`] : undefined,
    totalBytes: job.bytes_total,
    renditionCount: job.outputs.length,
    copyUrl: absoluteUrl(job.master_hls_path),
    copyLabel: "Copy adaptive master URL"
  }));
  return [...streamRows, ...sourceRows].sort((a, b) => {
    if (a.live !== b.live) return a.live ? -1 : 1;
    return (b.viewers ?? b.liveBitrateBps ?? 0) - (a.viewers ?? a.liveBitrateBps ?? 0);
  });
}

function LinksTable({ rows, onAction }: { rows: LinkRow[]; onAction: (stream: Stream, action: string) => void }) {
  if (!rows.length) {
    return <div className="empty-state"><Radio size={28} /><strong>No links yet</strong><span>Streams and source-transcode links will appear here automatically.</span></div>;
  }
  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>
            <th>Link</th><th>Status</th><th>Viewers</th><th>Bitrate</th><th>Bandwidth</th><th><span className="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          {rows.slice(0, 8).map((row) => (
            <tr key={row.key}>
              <td>
                <div className="stream-identity">
                  <span className="stream-avatar">{row.kind === "source" ? <Workflow size={17} /> : <Video size={17} />}</span>
                  <div>
                    <strong>{row.name}</strong>
                    <small>{row.application} / {row.name}{row.kind === "source" ? " · source transcode" : ""}</small>
                  </div>
                </div>
              </td>
              <td><StatusPill live={row.live} label={row.live ? "Live" : row.statusLabel} /></td>
              <td><strong className="viewer-count">{row.viewers === undefined ? "—" : compact(row.viewers)}</strong></td>
              <td>
                {row.liveBitrateBps !== undefined
                  ? `${bitrate(row.liveBitrateBps)}${row.renditionCount ? ` · ${row.renditionCount} renditions` : ""}`
                  : "—"}
              </td>
              <td>{row.totalBytes !== undefined ? bytes(row.totalBytes) : "—"}</td>
              <td>
                <div className="row-actions">
                  <IconButton label={row.copyLabel} onClick={() => copyText(row.copyUrl)}><Copy size={16} /></IconButton>
                  {row.stream && <IconButton label="More stream actions" onClick={() => onAction(row.stream!, "more")}><MoreHorizontal size={17} /></IconButton>}
                </div>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function ApplicationsPage({
  applications,
  streams,
  openCreate,
  onOpen
}: {
  applications: Application[];
  streams: Stream[];
  openCreate: () => void;
  onOpen: (application: Application) => void;
}) {
  return (
    <>
      <div className="section-intro">
        <div>
          <span className="eyebrow">APPLICATION LIBRARY</span>
          <h2>Your applications</h2>
          <p>{applications.length} {applications.length === 1 ? "application" : "applications"} created · grouped by namespace</p>
        </div>
        <button className="primary-button" onClick={openCreate}><Plus size={17} /> New application</button>
      </div>
      <section className="application-grid">
        {applications.map((app, index) => {
          const appStreams = streams.filter((stream) => stream.application === app.name);
          const live = appStreams.filter((stream) => stream.is_live).length;
          const viewers = appStreams.reduce((sum, stream) => sum + (stream.viewer_count ?? 0), 0);
          return (
            <button type="button" className="application-card" key={app.name} onClick={() => onOpen(app)}>
              <div className="app-card-head">
                <div className={`app-icon tone-${index % 4}`}><AppWindow size={20} /></div>
                <span className="app-card-status"><StatusPill live={app.enabled} label={app.enabled ? "Enabled" : "Disabled"} /><ChevronRight size={16} /></span>
              </div>
              <div className="app-card-title">
                <span>Application {String(index + 1).padStart(2, "0")}</span>
                <h3>{app.name}</h3>
              </div>
              <div className="app-endpoint"><RadioTower size={14} /><code>/{app.name}</code></div>
              <div className="app-stats">
                <div><strong>{appStreams.length}</strong><span>Streams</span></div>
                <div><strong className={live ? "live-number" : ""}>{live}</strong><span>Live now</span></div>
                <div><strong>{compact(viewers)}</strong><span>Viewers</span></div>
              </div>
            </button>
          );
        })}
        {!applications.length && <div className="empty-state full"><Layers3 size={30} /><strong>No applications</strong><span>Create the first namespace before adding a stream.</span></div>}
      </section>
    </>
  );
}

function ApplicationDetailPage({
  application,
  streams,
  templates,
  client,
  activeTab,
  setActiveTab,
  onBack,
  onNotify,
  onDeleteRequest
}: {
  application: Application;
  streams: Stream[];
  templates: TranscodingTemplate[];
  client: ControlClient;
  activeTab: ApplicationTab;
  setActiveTab: (tab: ApplicationTab) => void;
  onBack: () => void;
  onNotify: (type: "success" | "error", message: string) => void;
  onDeleteRequest: (application: Application) => void;
}) {
  const applicationStreams = streams.filter((stream) => stream.application === application.name);
  const [copiedUrl, setCopiedUrl] = useState<string | null>(null);
  const [assignments, setAssignments] = useState<TranscodingAssignment[]>([]);
  const [selectedTemplates, setSelectedTemplates] = useState<Record<string, string>>({});
  const [assignmentBusy, setAssignmentBusy] = useState<string | null>(null);
  const [assignmentsLoading, setAssignmentsLoading] = useState(false);
  const [sourceJobs, setSourceJobs] = useState<SourceTranscodeJob[]>([]);
  const [sourceUrl, setSourceUrl] = useState("");
  const [outputName, setOutputName] = useState("");
  const [sourceTemplateId, setSourceTemplateId] = useState("");
  const [sourceBusy, setSourceBusy] = useState(false);
  const [pendingDeleteJob, setPendingDeleteJob] = useState<SourceTranscodeJob | null>(null);
  const renditionStreamNames = new Set(assignments.flatMap((assignment) => assignment.outputs.map((output) => output.stream)));
  const sourceStreams = applicationStreams.filter((stream) => !renditionStreamNames.has(stream.name));

  const loadAssignments = useCallback(async () => {
    setAssignmentsLoading(true);
    try {
      const items = await client.listTranscodingAssignments(application.name);
      setAssignments(items);
      setSelectedTemplates((current) => {
        const next = { ...current };
        for (const item of items) {
          const template = templates.find((candidate) => candidate.name === item.template_name);
          if (template) next[item.source_stream] = template.id;
        }
        return next;
      });
    } catch (error) {
      onNotify("error", error instanceof Error ? error.message : "Could not load transcoding assignments.");
    } finally {
      setAssignmentsLoading(false);
    }
  }, [application.name, client, onNotify, templates]);

  useEffect(() => {
    if (activeTab === "transcoding") loadAssignments();
  }, [activeTab, loadAssignments]);

  const copyUrl = async (url: string) => {
    await copyText(url);
    setCopiedUrl(url);
    window.setTimeout(() => setCopiedUrl((current) => current === url ? null : current), 1500);
  };
  const applyTemplate = async (stream: Stream) => {
    const template = templates.find((item) => item.id === selectedTemplates[stream.name]);
    if (!template) {
      onNotify("error", "Select a transcoding template first.");
      return;
    }
    if (!template.presets.length) {
      onNotify("error", "This template has no encoding presets.");
      return;
    }
    const unsupported = template.presets.find((preset) =>
      !["H.264", "Passthrough", "Disabled"].includes(preset.videoCodec) ||
      !["AAC", "Passthrough", "Disabled"].includes(preset.audioCodec) ||
      ["Beamr"].includes(preset.implementation)
    );
    if (unsupported) {
      onNotify("error", `${unsupported.name} is not supported by the current RTMP/HLS output pipeline.`);
      return;
    }
    setAssignmentBusy(stream.name);
    try {
      const { rules, outputs } = buildTemplateRules(stream, template);
      const assignment = await client.assignTranscodingTemplate(stream, template.name, rules, outputs);
      setAssignments((current) => [
        ...current.filter((item) => item.source_stream !== stream.name),
        assignment
      ]);
      onNotify("success", `${template.name} applied. ${template.presets.length} resolutions now share one adaptive link.`);
    } catch (error) {
      onNotify("error", error instanceof Error ? error.message : "Could not apply the transcoding template.");
    } finally {
      setAssignmentBusy(null);
    }
  };
  const removeTemplate = async (stream: Stream) => {
    setAssignmentBusy(stream.name);
    try {
      await client.removeTranscodingAssignment(stream);
      setAssignments((current) => current.filter((item) => item.source_stream !== stream.name));
      setSelectedTemplates((current) => ({ ...current, [stream.name]: "" }));
      onNotify("success", `Transcoding removed from ${stream.name}.`);
    } catch (error) {
      onNotify("error", error instanceof Error ? error.message : "Could not remove transcoding.");
    } finally {
      setAssignmentBusy(null);
    }
  };

  const loadSourceJobs = useCallback(async () => {
    try {
      setSourceJobs(await client.listSourceTranscodes(application.name));
    } catch (error) {
      onNotify("error", error instanceof Error ? error.message : "Could not load source transcodes.");
    }
  }, [application.name, client, onNotify]);

  useEffect(() => {
    if (activeTab === "source") loadSourceJobs();
  }, [activeTab, loadSourceJobs]);

  const startSourceTranscode = async () => {
    const template = templates.find((item) => item.id === sourceTemplateId);
    const source = sourceUrl.trim();
    const name = safeRuleName(outputName, "");
    if (!source) {
      onNotify("error", "Enter a source URL (rtmp:// or an https .m3u8).");
      return;
    }
    if (!name) {
      onNotify("error", "Enter an output name for the transcoded stream.");
      return;
    }
    if (!template || !template.presets.length) {
      onNotify("error", "Select a transcoding template with at least one preset.");
      return;
    }
    const unsupported = template.presets.find((preset) =>
      !["H.264", "Passthrough", "Disabled"].includes(preset.videoCodec) ||
      !["AAC", "Passthrough", "Disabled"].includes(preset.audioCodec) ||
      preset.implementation !== "Default"
    );
    if (unsupported) {
      onNotify("error", `${unsupported.name} uses a codec/backend that is not built. Use H.264 + AAC on the Default backend.`);
      return;
    }
    setSourceBusy(true);
    try {
      const { rules, outputs } = buildSourceRules(application.name, name, template);
      const job = await client.createSourceTranscode(application.name, name, source, template.name, rules, outputs);
      setSourceJobs((current) => [...current.filter((item) => item.id !== job.id), job]);
      onNotify("success", `Transcoding ${source} into ${outputs.length} rendition${outputs.length === 1 ? "" : "s"}.`);
      setSourceUrl("");
      setOutputName("");
    } catch (error) {
      onNotify("error", error instanceof Error ? error.message : "Could not start the source transcode.");
    } finally {
      setSourceBusy(false);
    }
  };

  const toggleSourceTranscode = async (job: SourceTranscodeJob) => {
    setSourceBusy(true);
    try {
      const updated = await client.patchSourceTranscode(job, !job.enabled);
      setSourceJobs((current) => current.map((item) => (item.id === job.id ? updated : item)));
      onNotify("success", updated.enabled ? `Resumed ${job.name}.` : `Disabled ${job.name}. Source transcoding stopped.`);
    } catch (error) {
      onNotify("error", error instanceof Error ? error.message : "Could not update the source transcode.");
    } finally {
      setSourceBusy(false);
    }
  };

  const removeSourceTranscode = async (job: SourceTranscodeJob) => {
    setSourceBusy(true);
    try {
      await client.removeSourceTranscode(job);
      setSourceJobs((current) => current.filter((item) => item.id !== job.id));
      onNotify("success", `Stopped ${job.name}.`);
    } catch (error) {
      onNotify("error", error instanceof Error ? error.message : "Could not stop the source transcode.");
    } finally {
      setSourceBusy(false);
    }
  };

  const renderTab = () => {
    if (activeTab === "playback") {
      return (
        <section className="playback-workspace">
          <div className="playback-heading">
            <div><span className="eyebrow">AUTOMATICALLY GENERATED · NO TOKEN</span><h2>Playback URLs</h2><p>Every stream gets one direct RTMP URL and one smooth HLS .m3u8 playlist URL.</p></div>
            <span className="panel-count">{applicationStreams.length} streams</span>
          </div>
          <div className="playback-grid">
            {applicationStreams.map((stream) => {
              const hlsUrl = absoluteUrl(stream.hls_path);
              return (
                <article className="playback-card" key={`${stream.application}:${stream.name}`}>
                  <div className="playback-card-head">
                    <span className="stream-avatar"><Video size={17} /></span>
                    <div><strong>{stream.name}</strong><small>/{stream.application}/{stream.name}</small></div>
                    <StatusPill live={stream.is_live} />
                  </div>
                  <div className="playback-url-row">
                    <span>RTMP</span><code title={stream.rtmp_url}>{stream.rtmp_url}</code>
                    <IconButton label="Copy RTMP playback URL" onClick={() => copyUrl(stream.rtmp_url)}>
                      {copiedUrl === stream.rtmp_url ? <Check size={16} /> : <Copy size={16} />}
                    </IconButton>
                  </div>
                  <div className="playback-url-row recommended">
                    <span>M3U8</span><code title={hlsUrl}>{hlsUrl}</code>
                    <IconButton label="Copy HLS m3u8 playback URL" onClick={() => copyUrl(hlsUrl)}>
                      {copiedUrl === hlsUrl ? <Check size={16} /> : <Copy size={16} />}
                    </IconButton>
                  </div>
                  <div className="playback-card-foot"><Users size={14} /> {compact(stream.viewer_count ?? 0)} viewers <b>HLS recommended</b></div>
                </article>
              );
            })}
            {!applicationStreams.length && (
              <div className="empty-state full"><Radio size={28} /><strong>No playback URLs yet</strong><span>Playback links will appear when streams are registered for this application.</span></div>
            )}
          </div>
        </section>
      );
    }
    if (activeTab === "source") {
      return (
        <section className="assignment-workspace">
          <div className="playback-heading">
            <div><span className="eyebrow">PULL · TRANSCODE · RE-SERVE</span><h2>Transcode from a source URL</h2><p>Point at an external source (rtmp:// or an https .m3u8 carrying H.264/AAC). It is transcoded per the chosen template and re-served as one adaptive master .m3u8.</p></div>
            <span className="panel-count">{sourceJobs.length} running</span>
          </div>
          <article className="assignment-card source-form">
            <label className="assignment-select">Source URL
              <input
                type="text"
                inputMode="url"
                placeholder="rtmp://host/app/stream  or  https://host/live/index.m3u8"
                value={sourceUrl}
                onChange={(event) => setSourceUrl(event.target.value)}
              />
            </label>
            <label className="assignment-select">Output name
              <input
                type="text"
                placeholder="my-restream"
                value={outputName}
                onChange={(event) => setOutputName(event.target.value)}
              />
            </label>
            <label className="assignment-select">Transcoding template
              <select value={sourceTemplateId} onChange={(event) => setSourceTemplateId(event.target.value)}>
                <option value="">Select a template</option>
                {templates.map((template) => (
                  <option key={template.id} value={template.id}>{template.name} · {template.presets.length} resolutions</option>
                ))}
              </select>
            </label>
            <div className="assignment-actions">
              <button className="primary-button" disabled={sourceBusy} onClick={startSourceTranscode}>
                {sourceBusy ? <RefreshCw className="spin" size={16} /> : <Plus size={16} />}
                Start transcode
              </button>
            </div>
            {!templates.length && <div className="assignment-guidance"><Workflow size={18} /><span>Create a template from the sidebar Transcode page first.</span></div>}
          </article>
          <div className="assignment-grid">
            {sourceJobs.map((job) => {
              const masterUrl = absoluteUrl(job.master_hls_path);
              return (
                <article className="assignment-card assigned" key={job.id}>
                  <div className="assignment-card-head">
                    <span className="stream-avatar"><Workflow size={17} /></span>
                    <div><strong>{job.name}</strong><small title={job.source_url}>{job.source_url}</small></div>
                    <StatusPill live={job.status === "running"} label={job.status === "running" ? "Live" : job.enabled ? job.status : "Paused"} />
                  </div>
                  <div className="rendition-list">
                    {job.outputs.map((output) => (
                      <span key={output.stream}>
                        <b>{output.width && output.height ? `${output.width}×${output.height}` : output.name}</b>
                        <small>{bitrate(output.video_bitrate)} · {output.video_codec.toUpperCase()}</small>
                      </span>
                    ))}
                  </div>
                  {job.enabled ? (
                    <div className="master-url-row">
                      <span>MASTER M3U8</span><code title={masterUrl}>{masterUrl}</code>
                      <IconButton label="Copy adaptive master URL" onClick={() => copyUrl(masterUrl)}>
                        {copiedUrl === masterUrl ? <Check size={16} /> : <Copy size={16} />}
                      </IconButton>
                    </div>
                  ) : (
                    <div className="assignment-empty"><SlidersHorizontal size={19} /><span>Paused — the master link is offline until you resume it. The job itself is kept, not deleted.</span></div>
                  )}
                  {job.detail && <div className="assignment-empty"><Layers3 size={19} /><span>{job.detail}</span></div>}
                  <div className="assignment-actions">
                    <button className="secondary-button danger-text" disabled={sourceBusy} onClick={() => setPendingDeleteJob(job)}><Trash2 size={15} /> Delete</button>
                    <button className="secondary-button" disabled={sourceBusy} onClick={() => toggleSourceTranscode(job)}>
                      <SlidersHorizontal size={15} /> {job.enabled ? "Pause" : "Resume"}
                    </button>
                  </div>
                </article>
              );
            })}
            {!sourceJobs.length && <div className="empty-state full"><Radio size={28} /><strong>No source transcodes</strong><span>Add a source URL above to start transcoding into an adaptive ladder.</span></div>}
          </div>
        </section>
      );
    }
    return (
      <section className="assignment-workspace">
        <div className="playback-heading">
          <div><span className="eyebrow">ADAPTIVE BITRATE</span><h2>Assign transcoding templates</h2><p>Choose one template per source. Every preset becomes a resolution inside one master M3U8 link.</p></div>
          <span className="panel-count">{assignments.length} assigned</span>
        </div>
        {assignmentsLoading && <div className="assignment-loading"><RefreshCw className="spin" size={17} /> Loading assignments…</div>}
        <div className="assignment-grid">
          {sourceStreams.map((stream) => {
            const assignment = assignments.find((item) => item.source_stream === stream.name);
            const masterUrl = assignment ? absoluteUrl(assignment.master_hls_path) : "";
            return (
              <article className={`assignment-card ${assignment ? "assigned" : ""}`} key={`${stream.application}:${stream.name}`}>
                <div className="assignment-card-head">
                  <span className="stream-avatar"><Workflow size={17} /></span>
                  <div><strong>{stream.name}</strong><small>{assignment ? assignment.template_name : "No template assigned"}</small></div>
                  <StatusPill live={stream.is_live} />
                </div>
                <label className="assignment-select">Transcoding template
                  <select
                    value={selectedTemplates[stream.name] ?? ""}
                    onChange={(event) => setSelectedTemplates((current) => ({ ...current, [stream.name]: event.target.value }))}
                  >
                    <option value="">Select a template</option>
                    {templates.map((template) => (
                      <option key={template.id} value={template.id}>{template.name} · {template.presets.length} resolutions</option>
                    ))}
                  </select>
                </label>
                {assignment ? (
                  <>
                    <div className="rendition-list">
                      {assignment.outputs.map((output) => (
                        <span key={output.stream}>
                          <b>{output.width && output.height ? `${output.width}×${output.height}` : output.name}</b>
                          <small>{bitrate(output.video_bitrate)} · {output.video_codec.toUpperCase()}</small>
                        </span>
                      ))}
                    </div>
                    <div className="master-url-row">
                      <span>MASTER M3U8</span><code title={masterUrl}>{masterUrl}</code>
                      <IconButton label="Copy adaptive master URL" onClick={() => copyUrl(masterUrl)}>
                        {copiedUrl === masterUrl ? <Check size={16} /> : <Copy size={16} />}
                      </IconButton>
                    </div>
                  </>
                ) : (
                  <div className="assignment-empty"><Layers3 size={19} /><span>Select a template to create the adaptive resolution ladder.</span></div>
                )}
                <div className="assignment-actions">
                  {assignment && <button className="secondary-button danger-text" disabled={assignmentBusy === stream.name} onClick={() => removeTemplate(stream)}><Trash2 size={15} /> Remove</button>}
                  <button className="primary-button" disabled={!selectedTemplates[stream.name] || assignmentBusy === stream.name} onClick={() => applyTemplate(stream)}>
                    {assignmentBusy === stream.name ? <RefreshCw className="spin" size={16} /> : <Plus size={16} />}
                    {assignment ? "Update template" : "Add template"}
                  </button>
                </div>
              </article>
            );
          })}
          {!sourceStreams.length && <div className="empty-state full"><Radio size={28} /><strong>No source streams</strong><span>Create a stream before assigning a transcoding template.</span></div>}
        </div>
        {!templates.length && <div className="assignment-guidance"><Workflow size={18} /><span>Create a template from the sidebar Transcode page, add its resolution presets, then return here.</span></div>}
      </section>
    );
  };

  return (
    <div className="application-detail">
      <section className="application-detail-header">
        <div className="application-detail-title">
          <button className="back-button" type="button" onClick={onBack}><ArrowLeft size={17} /> Applications</button>
          <div className="application-title-row">
            <span className="app-icon tone-0"><AppWindow size={22} /></span>
            <div><span className="eyebrow">APPLICATION</span><h2>{application.name}</h2><p>RTMP namespace /{application.name}</p></div>
            <StatusPill live={application.enabled} label={application.enabled ? "Enabled" : "Disabled"} />
            <IconButton label="Delete application" danger onClick={() => onDeleteRequest(application)}><Trash2 size={16} /></IconButton>
          </div>
        </div>
        <div className="application-tabs" role="tablist" aria-label={`${application.name} sections`}>
          {applicationTabs.map((tab) => (
            <button
              type="button"
              role="tab"
              aria-selected={activeTab === tab.id}
              className={activeTab === tab.id ? "active" : ""}
              key={tab.id}
              onClick={() => setActiveTab(tab.id)}
            >
              {tab.label}
            </button>
          ))}
        </div>
      </section>
      <div className="application-tab-panel" role="tabpanel">{renderTab()}</div>
      {pendingDeleteJob && (
        <Modal
          title={`Delete ${pendingDeleteJob.name}?`}
          description={pendingDeleteJob.source_url}
          onClose={() => setPendingDeleteJob(null)}
        >
          <div className="delete-warning">
            <Trash2 size={21} />
            <div><strong>This cannot be undone.</strong><span>The source pull stops and its adaptive master link goes offline immediately.</span></div>
          </div>
          <div className="modal-actions">
            <button className="secondary-button" onClick={() => setPendingDeleteJob(null)}>Cancel</button>
            <button className="danger-button" onClick={() => {
              const job = pendingDeleteJob;
              setPendingDeleteJob(null);
              removeSourceTranscode(job);
            }}><Trash2 size={17} /> Delete source transcode</button>
          </div>
        </Modal>
      )}
    </div>
  );
}

function CapacityPage({
  bandwidth,
  setBandwidth,
  currentEgress,
  viewers,
  measuredBitrateMbps,
  measuredBitrateSource
}: {
  bandwidth: number;
  setBandwidth: (value: number) => void;
  currentEgress: number;
  viewers: number;
  measuredBitrateMbps: number;
  measuredBitrateSource: string;
}) {
  const [manualBitrateMbps, setManualBitrateMbps] = useState(1);
  const [autoBitrate, setAutoBitrate] = useState(true);
  const [headroom, setHeadroom] = useState(10);
  const [overhead, setOverhead] = useState(5);
  const hasMeasuredBitrate = measuredBitrateMbps > 0;
  const bitrateMbps = autoBitrate ? measuredBitrateMbps : manualBitrateMbps;
  const usable = bandwidth * (1 - headroom / 100);
  const perViewer = bitrateMbps > 0 ? bitrateMbps * (1 + overhead / 100) : 0;
  const theoretical = perViewer > 0 ? Math.floor(usable / perViewer) : null;
  const currentUse = bandwidth ? (currentEgress / 1e6 / bandwidth) * 100 : 0;
  return (
    <div className="capacity-grid">
      <section className="panel planner-panel">
        <div className="panel-heading"><div><span className="eyebrow">LINK BUDGET</span><h2>Direct-delivery calculator</h2></div><Network size={22} /></div>
        <p className="panel-lead">Estimate concurrent viewers when every byte leaves this VPS directly. This is a network ceiling—not a benchmark guarantee.</p>
        <div className="range-field">
          <div>
            <label htmlFor="bandwidth-number">Committed VPS bandwidth</label>
            <div className="unit-input bandwidth-entry">
              <input
                id="bandwidth-number"
                type="number"
                min="10"
                max="1000000"
                step="100"
                value={bandwidth}
                onChange={(event) => setBandwidth(Math.max(10, Math.min(1_000_000, Number(event.target.value) || 10)))}
              />
              <span>Mbps</span>
            </div>
          </div>
          <input id="bandwidth" aria-label="Committed VPS bandwidth slider" type="range" min="100" max="100000" step="100" value={Math.min(bandwidth, 100000)} onChange={(event) => setBandwidth(Number(event.target.value))} />
          <div className="range-bounds"><span>100 Mbps</span><span>100 Gbps · enter higher above</span></div>
        </div>
        <div className="planner-input-grid">
          <div className="planner-setting">
            <label htmlFor="stream-bitrate">Stream bitrate</label>
            <div className="unit-input">
              <input
                id="stream-bitrate"
                type="number"
                min=".01"
                step=".01"
                placeholder="Waiting for live media"
                disabled={autoBitrate}
                value={autoBitrate ? (hasMeasuredBitrate ? measuredBitrateMbps.toFixed(3) : "") : manualBitrateMbps}
                onChange={(event) => setManualBitrateMbps(Math.max(.01, Number(event.target.value) || .01))}
              />
              <span>Mbps</span>
            </div>
            <button type="button" className={`bitrate-mode ${autoBitrate ? "active" : ""}`} aria-pressed={autoBitrate} onClick={() => setAutoBitrate((current) => !current)}>
              <RadioTower size={13} /> {autoBitrate ? "Auto from live traffic" : "Use automatic bitrate"}
            </button>
            <small className="bitrate-source">
              {autoBitrate
                ? (hasMeasuredBitrate ? measuredBitrateSource : "Waiting for OBS or transcoder to publish media.")
                : "Manual planning only; the server still passes encoder bitrate through unchanged."}
            </small>
          </div>
          <label>Safety headroom<div className="unit-input"><input type="number" min="5" max="50" value={headroom} onChange={(event) => setHeadroom(Number(event.target.value))} /><span>%</span></div></label>
          <label>Protocol overhead<div className="unit-input"><input type="number" min="1" max="20" value={overhead} onChange={(event) => setOverhead(Number(event.target.value))} /><span>%</span></div></label>
        </div>
        <div className="formula">
          <code>{perViewer > 0 ? `(${bandwidth} × ${(1 - headroom / 100).toFixed(2)}) ÷ (${bitrateMbps.toFixed(3)} × ${(1 + overhead / 100).toFixed(2)})` : "Waiting for a live publisher…"}</code>
          <span>{perViewer > 0 ? "usable link ÷ delivered bitrate per viewer" : "capacity appears after media starts flowing"}</span>
        </div>
      </section>
      <aside className="capacity-result">
        <span className="eyebrow">ESTIMATED CEILING</span>
        <strong>{theoretical === null ? "—" : theoretical.toLocaleString()}</strong>
        <h2>concurrent viewers</h2>
        <p>{bitrateMbps > 0 ? `at ${bitrateMbps.toFixed(3)} Mbps average, with ${headroom}% capacity held for bursts.` : "Start publishing from OBS or the transcoder to calculate from measured bitrate."}</p>
        <div className="result-divider" />
        <div className="result-row"><span>Usable delivery</span><b>{usable.toLocaleString()} Mbps</b></div>
        <div className="result-row"><span>Per-viewer budget</span><b>{perViewer > 0 ? `${perViewer.toFixed(3)} Mbps` : "Waiting"}</b></div>
        <div className="result-row"><span>Current viewers</span><b>{viewers.toLocaleString()}</b></div>
        <div className="result-row"><span>Current uplink use</span><b>{currentUse.toFixed(1)}%</b></div>
        <div className="capacity-warning"><Activity size={18} /><span>CPU, kernel send cost, socket memory and NIC PPS can become the limit before bandwidth. Validate with the included load generator.</span></div>
      </aside>
      <section className="panel capacity-guidance">
        <div><span className="guidance-icon"><Router size={20} /></span><h3>Fair queueing</h3><p>The installer applies CAKE on the detected public interface using the detected or declared link rate.</p></div>
        <div><span className="guidance-icon"><CircleGauge size={20} /></span><h3>Keep headroom</h3><p>The high-density default reserves 10%. Use 15–25% when traffic is bursty or the provider's bandwidth is not guaranteed.</p></div>
        <div><span className="guidance-icon"><CloudOff size={20} /></span><h3>No CDN multiplier</h3><p>Every viewer consumes origin egress. Ten thousand viewers require sub-1 Mbps delivery on a 10Gbps link.</p></div>
      </section>
    </div>
  );
}

function SystemPage({ snapshot }: { snapshot: Snapshot }) {
  const items = [
    ["RTMP transport", "io_uring / SO_REUSEPORT", "Operational", <Zap size={19} />],
    ["Control database", "SQLite WAL · local disk", "Ready", <Database size={19} />],
    ["Management API", "Direct panel access", "Open", <ShieldCheck size={19} />],
    ["Media delivery", "Direct origin · no CDN", "Active", <Network size={19} />]
  ];
  return (
    <>
      <section className="system-banner">
        <div className="system-orb"><Server size={28} /></div>
        <div><span className="eyebrow">ORIGIN NODE</span><h2>RTMP server is {snapshot.health === "online" ? "operational" : "degraded"}</h2><p>Management plane and telemetry are responding from this node.</p></div>
        <StatusPill live={snapshot.health === "online"} label={snapshot.health === "online" ? "All systems normal" : "Check service"} />
      </section>
      <div className="system-grid">
        <section className="panel">
          <div className="panel-heading"><div><span className="eyebrow">COMPONENTS</span><h2>Runtime services</h2></div></div>
          <div className="component-list">
            {items.map(([name, detail, status, icon]) => (
              <div key={name as string}><span className="component-icon">{icon}</span><p><strong>{name}</strong><small>{detail}</small></p><b>{status}</b></div>
            ))}
          </div>
        </section>
        <section className="panel">
          <div className="panel-heading"><div><span className="eyebrow">RESOURCE SNAPSHOT</span><h2>Process telemetry</h2></div></div>
          <div className="telemetry-grid">
            <div><span>CPU usage</span><strong>{(metric(snapshot, "worker_cpu_usage") / ((metric(snapshot, "cpu_cores_available") || 1) * 10)).toFixed(1)}%</strong></div>
            <div><span>CPU cores</span><strong>{compact(metric(snapshot, "cpu_cores_available") || 1)}</strong></div>
            <div><span>Memory RSS</span><strong>{bytes(metric(snapshot, "process_memory_bytes"))}</strong></div>
            <div><span>GOP cache</span><strong>{bytes(metric(snapshot, "gop_cache_bytes"))}</strong></div>
            <div><span>Egress total</span><strong>{bytes(metric(snapshot, "egress_bytes_total"))}</strong></div>
            <div><span>Ingress total</span><strong>{bytes(metric(snapshot, "ingress_bytes_total"))}</strong></div>
            <div><span>Dropped video</span><strong>{compact(metric(snapshot, "dropped_video_frames"))}</strong></div>
            <div><span>Active streams</span><strong>{compact(metric(snapshot, "active_streams"))}</strong></div>
          </div>
        </section>
      </div>
      <section className="panel config-note">
        <Settings2 size={21} />
        <div><strong>Server-owned configuration</strong><span>Kernel limits, listener queues, worker count and interface shaping are managed by the Linux installer in <code>/etc/rtmp-server</code>.</span></div>
      </section>
    </>
  );
}

function NewTemplateModal({
  onClose,
  onAdd,
  editing
}: {
  onClose: () => void;
  onAdd: (name: string) => void;
  editing?: TranscodingTemplate | null;
}) {
  const [name, setName] = useState(editing?.name ?? "");
  return (
    <Modal title={editing ? "Rename template" : "Add new template"} description="Give this transcoding template a clear name." onClose={onClose}>
      <form className="modal-form" onSubmit={(event: FormEvent) => {
        event.preventDefault();
        onAdd(name.trim());
      }}>
        <label htmlFor="template-name">Template Name *
          <input id="template-name" autoFocus required value={name} onChange={(event) => setName(event.target.value)} placeholder="For example: Sports HD" />
          <small>You can add one or more encoding presets inside this template.</small>
        </label>
        <div className="modal-actions">
          <button type="button" className="secondary-button" onClick={onClose}>Cancel</button>
          <button className="primary-button" disabled={!name.trim()}><Plus size={17} /> {editing ? "Save changes" : "Add template"}</button>
        </div>
      </form>
    </Modal>
  );
}

function NewPresetModal({
  onClose,
  onAdd,
  editing
}: {
  onClose: () => void;
  onAdd: (preset: EncodingPreset) => void;
  editing?: EncodingPreset | null;
}) {
  const [name, setName] = useState(editing?.name ?? "");
  const [outgoingStreamName, setOutgoingStreamName] = useState(editing?.outgoingStreamName ?? "");
  const [description, setDescription] = useState(editing?.description ?? "");
  const [videoCodec, setVideoCodec] = useState<VideoCodec>(editing?.videoCodec ?? "H.264");
  const [videoBitrateKbps, setVideoBitrateKbps] = useState(editing ? String(editing.videoBitrate / 1000) : "2500");
  const [implementation, setImplementation] = useState<EncodingImplementation>(editing?.implementation ?? "Default");
  const [profile, setProfile] = useState<VideoProfile>(editing?.profile ?? "high");
  const [keyFrameMode, setKeyFrameMode] = useState<"source" | "interval">(editing?.keyFrameMode ?? "source");
  const [keyFrameInterval, setKeyFrameInterval] = useState(editing?.keyFrameInterval ? String(editing.keyFrameInterval) : "60");
  const initialResolution = editing?.frameWidth && editing?.frameHeight
    ? (frameResolutions.find((item) => item.width === editing.frameWidth && item.height === editing.frameHeight)?.id ?? "custom")
    : "source";
  const [frameResolution, setFrameResolution] = useState<FrameResolutionId>(initialResolution);
  const [frameWidth, setFrameWidth] = useState(initialResolution === "custom" ? String(editing?.frameWidth ?? "") : "");
  const [frameHeight, setFrameHeight] = useState(initialResolution === "custom" ? String(editing?.frameHeight ?? "") : "");
  const [fitMode, setFitMode] = useState<FitMode>(editing?.fitMode && editing.fitMode !== "match-source" ? editing.fitMode : "letterbox");
  const [audioCodec, setAudioCodec] = useState<AudioCodec>(editing?.audioCodec ?? "AAC");
  const [audioBitrateKbps, setAudioBitrateKbps] = useState(editing ? String(editing.audioBitrate / 1000) : "128");
  const [gpuMode, setGpuMode] = useState<"first" | "specific">(editing?.gpuMode ?? "first");
  const [gpuId, setGpuId] = useState(editing?.gpuId !== undefined && editing?.gpuId !== null ? String(editing.gpuId) : "0");
  const selectedResolution = frameResolutions.find((resolution) => resolution.id === frameResolution) ?? frameResolutions[0];
  const sourceResolution = frameResolution === "source";
  const customResolution = frameResolution === "custom";
  const customWidth = Number(frameWidth);
  const customHeight = Number(frameHeight);
  const validCustomDimensions = !customResolution || (
    Number.isInteger(customWidth) && customWidth >= 16 && customWidth <= 8192 && customWidth % 2 === 0 &&
    Number.isInteger(customHeight) && customHeight >= 16 && customHeight <= 8192 && customHeight % 2 === 0
  );
  const outputWidth = customResolution ? customWidth : selectedResolution.width;
  const outputHeight = customResolution ? customHeight : selectedResolution.height;
  return (
    <Modal title={editing ? "Edit encoding preset" : "Add encoding preset"} description="Configure the outgoing video encoding profile." onClose={onClose} wide>
      <form className="modal-form preset-form" onSubmit={(event: FormEvent) => {
        event.preventDefault();
        onAdd({
          id: editing?.id ?? newLocalId(),
          enabled: editing?.enabled ?? true,
          name: name.trim(),
          outgoingStreamName: outgoingStreamName.trim(),
          description: description.trim(),
          videoCodec,
          videoBitrate: Math.max(0, Number(videoBitrateKbps) || 0) * 1000,
          implementation,
          profile,
          keyFrameMode,
          keyFrameInterval: keyFrameMode === "interval" ? Math.max(1, Number(keyFrameInterval) || 1) : null,
          frameWidth: sourceResolution ? null : outputWidth,
          frameHeight: sourceResolution ? null : outputHeight,
          fitMode: sourceResolution ? "match-source" : fitMode,
          audioCodec,
          audioBitrate: Math.max(0, Number(audioBitrateKbps) || 0) * 1000,
          gpuMode,
          gpuId: gpuMode === "specific" ? Math.max(0, Number(gpuId) || 0) : null
        });
      }}>
        <div className="two-fields preset-basics">
          <label htmlFor="preset-name">Preset Name *
            <input id="preset-name" autoFocus required value={name} onChange={(event) => setName(event.target.value)} placeholder="1080p Main" />
          </label>
          <label htmlFor="outgoing-stream-name">Outgoing Stream Name *
            <input id="outgoing-stream-name" required value={outgoingStreamName} onChange={(event) => setOutgoingStreamName(event.target.value)} placeholder="live_1080p" />
          </label>
        </div>
        <label htmlFor="preset-description">Description
          <textarea id="preset-description" value={description} onChange={(event) => setDescription(event.target.value)} placeholder="Describe where this preset will be used…" />
        </label>

        <div className="form-section-heading"><span><Video size={17} /></span><div><strong>Video Settings</strong><small>Codec, bitrate and encoding hardware</small></div></div>
        <div className="preset-video-grid">
          <label htmlFor="video-codec">Video Codec
            <select id="video-codec" value={videoCodec} onChange={(event) => setVideoCodec(event.target.value as VideoCodec)}>
              {videoCodecs.map((codec) => <option key={codec} value={codec} disabled={!builtVideoCodecs.has(codec)}>{codec}{builtVideoCodecs.has(codec) ? "" : " — not built"}</option>)}
            </select>
          </label>
          <label htmlFor="video-bitrate">Video Bitrate
            <div className="field-with-unit">
              <input id="video-bitrate" type="number" min="0" step="100" value={videoBitrateKbps} onChange={(event) => setVideoBitrateKbps(event.target.value)} />
              <span>kbps</span>
            </div>
            <small>Kilobits per second (kbps)</small>
          </label>
          <label htmlFor="encoding-implementation">Encoding Implementation
            <select id="encoding-implementation" value={implementation} onChange={(event) => setImplementation(event.target.value as EncodingImplementation)}>
              {encodingImplementations.map((item) => <option key={item} value={item} disabled={!builtImplementations.has(item)}>{item}{builtImplementations.has(item) ? "" : " — not built"}</option>)}
            </select>
          </label>
          <label htmlFor="video-profile">Profile
            <select id="video-profile" value={profile} onChange={(event) => setProfile(event.target.value as VideoProfile)}>
              {videoProfiles.map((item) => <option key={item}>{item}</option>)}
            </select>
          </label>
        </div>

        <fieldset className="gpu-fieldset keyframe-fieldset">
          <legend>Key Frame Interval</legend>
          <label className="radio-option">
            <input type="radio" name="key-frame-mode" checked={keyFrameMode === "source"} onChange={() => setKeyFrameMode("source")} />
            <span><strong>Same as source</strong><small>Required for transrating.</small></span>
          </label>
          <label className="radio-option">
            <input type="radio" name="key-frame-mode" checked={keyFrameMode === "interval"} onChange={() => setKeyFrameMode("interval")} />
            <span><strong>Insert a key frame every:</strong><small>Set a fixed frame interval.</small></span>
            <div className="compact-unit-input">
              <input aria-label="Key frame interval" type="number" min="1" value={keyFrameInterval} disabled={keyFrameMode !== "interval"} onChange={(event) => setKeyFrameInterval(event.target.value)} />
              <span>frames</span>
            </div>
          </label>
        </fieldset>

        <div className="form-section-heading"><span><AppWindow size={17} /></span><div><strong>Frame Size</strong><small>Output dimensions and source fitting</small></div></div>
        <div className={`frame-size-grid${customResolution ? " custom-resolution" : ""}`}>
          <label htmlFor="frame-resolution">Resolution
            <select id="frame-resolution" value={frameResolution} onChange={(event) => {
              const next = event.target.value as FrameResolutionId;
              setFrameResolution(next);
              if (next !== "custom") {
                setFrameWidth("");
                setFrameHeight("");
              }
            }}>
              {frameResolutions.map((resolution) => <option key={resolution.id} value={resolution.id}>{resolution.label}</option>)}
            </select>
            <small>{sourceResolution ? "Keep the input frame dimensions." : "The encoder output will use this exact frame size."}</small>
          </label>
          {customResolution && <>
            <label htmlFor="frame-width">Width
              <div className="field-with-unit"><input id="frame-width" type="number" min="16" max="8192" step="2" required placeholder="1280" value={frameWidth} onChange={(event) => setFrameWidth(event.target.value)} /><span>px</span></div>
            </label>
            <label htmlFor="frame-height">Height
              <div className="field-with-unit"><input id="frame-height" type="number" min="16" max="8192" step="2" required placeholder="720" value={frameHeight} onChange={(event) => setFrameHeight(event.target.value)} /><span>px</span></div>
              <small>Use even values from 16 to 8192 pixels.</small>
            </label>
          </>}
          <label htmlFor="fit-mode">Fit Mode
            <select id="fit-mode" value={sourceResolution ? "match-source" : fitMode} disabled={sourceResolution} onChange={(event) => setFitMode(event.target.value as FitMode)}>
              {sourceResolution
                ? <option value="match-source">{fitModeLabels["match-source"]}</option>
                : outputFitModes.map((item) => <option key={item} value={item}>{fitModeLabels[item]}</option>)}
            </select>
            <small>{sourceResolution ? "No scaling is applied." : "Letterbox preserves aspect ratio and guarantees the selected output size."}</small>
          </label>
        </div>

        <fieldset className="gpu-fieldset">
          <legend>GPU ID</legend>
          <label className="radio-option">
            <input type="radio" name="gpu-mode" checked={gpuMode === "first"} onChange={() => setGpuMode("first")} />
            <span><strong>Use first available GPU</strong><small>Automatically select an available compatible GPU.</small></span>
          </label>
          <label className="radio-option">
            <input type="radio" name="gpu-mode" checked={gpuMode === "specific"} onChange={() => setGpuMode("specific")} />
            <span><strong>Use GPU ID:</strong><small>Select one specific GPU for this preset.</small></span>
            <input className="gpu-id-input" aria-label="GPU ID" type="number" min="0" value={gpuId} disabled={gpuMode !== "specific"} onChange={(event) => setGpuId(event.target.value)} />
          </label>
        </fieldset>

        <div className="form-section-heading"><span><Radio size={17} /></span><div><strong>Audio Settings</strong><small>Codec and outgoing audio bitrate</small></div></div>
        <div className="audio-settings-grid">
          <label htmlFor="audio-codec">Audio Codec
            <select id="audio-codec" value={audioCodec} onChange={(event) => setAudioCodec(event.target.value as AudioCodec)}>
              {audioCodecs.map((codec) => <option key={codec} value={codec} disabled={!builtAudioCodecs.has(codec)}>{codec}{builtAudioCodecs.has(codec) ? "" : " — not built"}</option>)}
            </select>
          </label>
          <label htmlFor="audio-bitrate">Audio Bitrate
            <div className="field-with-unit">
              <input id="audio-bitrate" type="number" min="0" step="8" value={audioBitrateKbps} onChange={(event) => setAudioBitrateKbps(event.target.value)} />
              <span>kbps</span>
            </div>
            <small>Kilobits per second (kbps)</small>
          </label>
        </div>

        <div className="modal-actions">
          <button type="button" className="secondary-button" onClick={onClose}>Cancel</button>
          <button className="primary-button" disabled={!name.trim() || !outgoingStreamName.trim() || !validCustomDimensions}><Plus size={17} /> {editing ? "Save preset" : "Add preset"}</button>
        </div>
      </form>
    </Modal>
  );
}

function TranscodePage({
  templates,
  client,
  onNotify,
  refreshTemplates
}: {
  templates: TranscodingTemplate[];
  client: ControlClient;
  onNotify: (type: "success" | "error", message: string) => void;
  refreshTemplates: () => Promise<void>;
}) {
  const [selectedTemplateId, setSelectedTemplateId] = useState<string | null>(null);
  const [showTemplateModal, setShowTemplateModal] = useState(false);
  const [renamingTemplate, setRenamingTemplate] = useState<TranscodingTemplate | null>(null);
  const [pendingDeleteTemplate, setPendingDeleteTemplate] = useState<TranscodingTemplate | null>(null);
  const [showPresetModal, setShowPresetModal] = useState(false);
  const [editingPreset, setEditingPreset] = useState<EncodingPreset | null>(null);
  const [pendingDeletePreset, setPendingDeletePreset] = useState<EncodingPreset | null>(null);
  const [busy, setBusy] = useState(false);
  const selectedTemplate = templates.find((template) => template.id === selectedTemplateId) ?? null;

  const errorMessage = (error: unknown, fallback: string) => (error instanceof Error ? error.message : fallback);

  const addTemplate = async (name: string) => {
    setBusy(true);
    try {
      await client.putTemplate(newLocalId(), name, []);
      await refreshTemplates();
      setShowTemplateModal(false);
      onNotify("success", "Template created.");
    } catch (error) {
      onNotify("error", errorMessage(error, "Could not create the template."));
    } finally {
      setBusy(false);
    }
  };
  const renameTemplate = async (name: string) => {
    if (!renamingTemplate) return;
    setBusy(true);
    try {
      await client.putTemplate(renamingTemplate.id, name, renamingTemplate.presets);
      await refreshTemplates();
      setRenamingTemplate(null);
      onNotify("success", "Template renamed.");
    } catch (error) {
      onNotify("error", errorMessage(error, "Could not rename the template."));
    } finally {
      setBusy(false);
    }
  };
  const deleteTemplate = async () => {
    if (!pendingDeleteTemplate) return;
    setBusy(true);
    try {
      await client.deleteTemplate(pendingDeleteTemplate.id);
      if (selectedTemplateId === pendingDeleteTemplate.id) setSelectedTemplateId(null);
      setPendingDeleteTemplate(null);
      await refreshTemplates();
      onNotify("success", "Template deleted.");
    } catch (error) {
      onNotify("error", errorMessage(error, "Could not delete the template."));
    } finally {
      setBusy(false);
    }
  };
  const savePreset = async (preset: EncodingPreset) => {
    if (!selectedTemplate) return;
    const exists = selectedTemplate.presets.some((item) => item.id === preset.id);
    const nextPresets = exists
      ? selectedTemplate.presets.map((item) => (item.id === preset.id ? preset : item))
      : [...selectedTemplate.presets, preset];
    setBusy(true);
    try {
      await client.putTemplate(selectedTemplate.id, selectedTemplate.name, nextPresets);
      await refreshTemplates();
      setShowPresetModal(false);
      setEditingPreset(null);
    } catch (error) {
      onNotify("error", errorMessage(error, "Could not save the preset."));
    } finally {
      setBusy(false);
    }
  };
  const addFullLadder = async () => {
    if (!selectedTemplate) return;
    const existingResolutions = new Set(
      selectedTemplate.presets.map((preset) => `${preset.frameWidth}x${preset.frameHeight}`)
    );
    const additions = fullAdaptiveLadderPresets().filter(
      (preset) => !existingResolutions.has(`${preset.frameWidth}x${preset.frameHeight}`)
    );
    if (additions.length === 0) {
      onNotify("success", "The full 144p–2160p ladder is already configured.");
      return;
    }
    setBusy(true);
    try {
      await client.putTemplate(
        selectedTemplate.id,
        selectedTemplate.name,
        [...selectedTemplate.presets, ...additions]
      );
      await refreshTemplates();
      onNotify("success", `${additions.length} missing ladder preset${additions.length === 1 ? "" : "s"} added.`);
    } catch (error) {
      onNotify("error", errorMessage(error, "Could not add the full adaptive ladder."));
    } finally {
      setBusy(false);
    }
  };
  const togglePreset = async (preset: EncodingPreset) => {
    if (!selectedTemplate) return;
    const nextPresets = selectedTemplate.presets.map((item) =>
      item.id === preset.id ? { ...item, enabled: !item.enabled } : item
    );
    setBusy(true);
    try {
      await client.putTemplate(selectedTemplate.id, selectedTemplate.name, nextPresets);
      await refreshTemplates();
    } catch (error) {
      onNotify("error", errorMessage(error, "Could not update the preset."));
    } finally {
      setBusy(false);
    }
  };
  const deletePreset = async () => {
    if (!selectedTemplate || !pendingDeletePreset) return;
    const nextPresets = selectedTemplate.presets.filter((item) => item.id !== pendingDeletePreset.id);
    setBusy(true);
    try {
      await client.putTemplate(selectedTemplate.id, selectedTemplate.name, nextPresets);
      setPendingDeletePreset(null);
      await refreshTemplates();
      onNotify("success", "Preset deleted.");
    } catch (error) {
      onNotify("error", errorMessage(error, "Could not delete the preset."));
    } finally {
      setBusy(false);
    }
  };

  if (selectedTemplate) {
    return (
      <div className="template-detail">
        <section className="template-detail-header">
          <button className="back-button" type="button" onClick={() => setSelectedTemplateId(null)}><ArrowLeft size={17} /> Transcoding templates</button>
          <div className="template-title-row">
            <span className="template-large-icon"><Workflow size={24} /></span>
            <div><span className="eyebrow">TRANSCODING TEMPLATE</span><h2>{selectedTemplate.name}</h2><p>{selectedTemplate.presets.length} encoding presets configured</p></div>
            <div className="row-actions">
              <IconButton label="Rename template" onClick={() => !busy && setRenamingTemplate(selectedTemplate)}><Settings2 size={16} /></IconButton>
              <IconButton label="Delete template" danger onClick={() => !busy && setPendingDeleteTemplate(selectedTemplate)}><Trash2 size={16} /></IconButton>
            </div>
          </div>
          <div className="template-tabs"><button className="active" type="button">Encoding Presets</button></div>
        </section>
        <section className="preset-workspace">
          <div className="preset-workspace-heading">
            <div><span className="eyebrow">OUTPUT PROFILES</span><h2>Encoding Presets</h2><p>Create one preset for every outgoing stream rendition.</p></div>
            <div className="row-actions">
              <button className="secondary-button" disabled={busy} onClick={addFullLadder}><Workflow size={17} /> Add Full Ladder</button>
              <button className="primary-button" disabled={busy} onClick={() => setShowPresetModal(true)}><Plus size={17} /> Add Preset</button>
            </div>
          </div>
          <div className="preset-grid">
            {selectedTemplate.presets.map((preset) => (
              <article className={`preset-card${preset.enabled ? "" : " preset-disabled"}`} key={preset.id}>
                <div className="preset-card-head">
                  <span><Video size={18} /></span>
                  <div><strong>{preset.name}</strong><small>{preset.outgoingStreamName}</small></div>
                  <b>{preset.videoCodec}</b>
                  <div className="row-actions">
                    <IconButton label={preset.enabled ? "Disable preset" : "Enable preset"} onClick={() => !busy && togglePreset(preset)}><SlidersHorizontal size={15} /></IconButton>
                    <IconButton label="Edit preset" onClick={() => { if (!busy) { setEditingPreset(preset); setShowPresetModal(true); } }}><Settings2 size={15} /></IconButton>
                    <IconButton label="Delete preset" danger onClick={() => !busy && setPendingDeletePreset(preset)}><Trash2 size={15} /></IconButton>
                  </div>
                </div>
                <span className={preset.enabled ? "enabled-text" : "disabled-text"}>{preset.enabled ? "Enabled" : "Disabled"}</span>
                {preset.description && <p>{preset.description}</p>}
                <div className="preset-card-stats">
                  <div><span>Video bitrate</span><strong>{preset.videoBitrate ? `${(preset.videoBitrate / 1000).toLocaleString()} kbps` : "Automatic"}</strong></div>
                  <div><span>Implementation</span><strong>{preset.implementation}</strong></div>
                  <div><span>Profile</span><strong>{preset.profile}</strong></div>
                  <div><span>Key frames</span><strong>{preset.keyFrameMode === "source" ? "Same as source" : `Every ${preset.keyFrameInterval} frames`}</strong></div>
                  <div><span>Frame size</span><strong>{preset.frameWidth && preset.frameHeight ? `${preset.frameWidth} × ${preset.frameHeight}` : "Same as source"} · {preset.fitMode}</strong></div>
                  <div><span>Audio</span><strong>{preset.audioCodec} · {(preset.audioBitrate / 1000).toLocaleString()} kbps</strong></div>
                  <div><span>GPU</span><strong>{preset.gpuMode === "first" ? "First available" : `GPU ${preset.gpuId}`}</strong></div>
                </div>
              </article>
            ))}
            {!selectedTemplate.presets.length && (
              <div className="empty-state full preset-empty"><SlidersHorizontal size={30} /><strong>No encoding presets</strong><span>Add the first output profile for this template.</span></div>
            )}
          </div>
        </section>
        {showPresetModal && (
          <NewPresetModal
            editing={editingPreset}
            onClose={() => { setShowPresetModal(false); setEditingPreset(null); }}
            onAdd={savePreset}
          />
        )}
        {renamingTemplate && (
          <NewTemplateModal editing={renamingTemplate} onClose={() => setRenamingTemplate(null)} onAdd={renameTemplate} />
        )}
        {pendingDeleteTemplate && (
          <Modal
            title={`Delete ${pendingDeleteTemplate.name}?`}
            description="This removes the template and all of its encoding presets from the server."
            onClose={() => setPendingDeleteTemplate(null)}
          >
            <div className="modal-actions">
              <button type="button" className="secondary-button" disabled={busy} onClick={() => setPendingDeleteTemplate(null)}>Cancel</button>
              <button type="button" className="danger-button" disabled={busy} onClick={deleteTemplate}><Trash2 size={17} /> Delete template</button>
            </div>
          </Modal>
        )}
        {pendingDeletePreset && (
          <Modal
            title={`Delete ${pendingDeletePreset.name}?`}
            description="This removes the preset from this template."
            onClose={() => setPendingDeletePreset(null)}
          >
            <div className="modal-actions">
              <button type="button" className="secondary-button" disabled={busy} onClick={() => setPendingDeletePreset(null)}>Cancel</button>
              <button type="button" className="danger-button" disabled={busy} onClick={deletePreset}><Trash2 size={17} /> Delete preset</button>
            </div>
          </Modal>
        )}
      </div>
    );
  }

  return (
    <div className="transcode-template-page">
      <div className="section-intro">
        <div><span className="eyebrow">TRANSCODING</span><h2>Templates</h2><p>Create reusable encoding presets for your outgoing streams.</p></div>
        <button className="primary-button" disabled={busy} onClick={() => setShowTemplateModal(true)}><Plus size={17} /> Add New Template</button>
      </div>
      <section className="template-grid">
        {templates.map((template, index) => (
          <div className="template-card" key={template.id} onClick={() => setSelectedTemplateId(template.id)} role="button" tabIndex={0}
            onKeyDown={(event) => { if (event.key === "Enter") setSelectedTemplateId(template.id); }}>
            <div className={`template-icon tone-${index % 4}`}><Workflow size={22} /></div>
            <span className="template-open"><ChevronRight size={17} /></span>
            <div className="template-card-actions row-actions" onClick={(event) => event.stopPropagation()}>
              <IconButton label="Rename template" onClick={() => !busy && setRenamingTemplate(template)}><Settings2 size={15} /></IconButton>
              <IconButton label="Delete template" danger onClick={() => !busy && setPendingDeleteTemplate(template)}><Trash2 size={15} /></IconButton>
            </div>
            <span className="template-label">Template {String(index + 1).padStart(2, "0")}</span>
            <h3>{template.name}</h3>
            <p>{template.presets.length ? `${template.presets.length} encoding presets` : "No presets configured yet"}</p>
            <div className="template-card-foot"><span><Cpu size={14} /> Encoding template</span><b>{template.presets.length}</b></div>
          </div>
        ))}
        {!templates.length && (
          <div className="empty-state full template-empty"><Workflow size={31} /><strong>No transcoding templates</strong><span>Create a template, then add one or more encoding presets.</span><button className="primary-button" disabled={busy} onClick={() => setShowTemplateModal(true)}><Plus size={17} /> Add New Template</button></div>
        )}
      </section>
      {showTemplateModal && <NewTemplateModal onClose={() => setShowTemplateModal(false)} onAdd={addTemplate} />}
      {renamingTemplate && (
        <NewTemplateModal editing={renamingTemplate} onClose={() => setRenamingTemplate(null)} onAdd={renameTemplate} />
      )}
      {pendingDeleteTemplate && (
        <Modal
          title={`Delete ${pendingDeleteTemplate.name}?`}
          description="This removes the template and all of its encoding presets from the server."
          onClose={() => setPendingDeleteTemplate(null)}
        >
          <div className="modal-actions">
            <button type="button" className="secondary-button" disabled={busy} onClick={() => setPendingDeleteTemplate(null)}>Cancel</button>
            <button type="button" className="danger-button" disabled={busy} onClick={deleteTemplate}><Trash2 size={17} /> Delete template</button>
          </div>
        </Modal>
      )}
      <div className="storage-note"><Database size={16} /><span>Templates are stored on the server (SQLite) and shared across every admin session.</span></div>
    </div>
  );
}

function App() {
  const demo = new URLSearchParams(window.location.search).get("demo") === "1";
  const [client] = useState(() => new ControlClient(demo));
  const [snapshot, setSnapshot] = useState<Snapshot>(EMPTY_SNAPSHOT);
  const [page, setPage] = useState<Page>("home");
  const [selectedApplication, setSelectedApplication] = useState<Application | null>(null);
  const [pendingDeleteApplication, setPendingDeleteApplication] = useState<Application | null>(null);
  const [applicationTab, setApplicationTab] = useState<ApplicationTab>("playback");
  const [loading, setLoading] = useState(false);
  const [notice, setNotice] = useState<Notice>(null);
  const [mobileNav, setMobileNav] = useState(false);
  const [createType, setCreateType] = useState<"application" | null>(null);
  const [actionStream, setActionStream] = useState<Stream | null>(null);
  const [pendingDelete, setPendingDelete] = useState<Stream | null>(null);
  const [templates, setTemplates] = useState<TranscodingTemplate[]>([]);
  const [bandwidth, setBandwidthState] = useState(() => Number(localStorage.getItem("streamforge-bandwidth")) || 10000);
  const [history, setHistory] = useState<number[]>(demo ? [5200, 5400, 5310, 5710, 5890, 6030, 6210, 6150, 6420, 6615] : []);
  // Per-stream egress bitrate, derived on the client from the delta of
  // egress_bytes_total between two polls — the backend only exposes the
  // cumulative counter (see admin/src/api.ts Stream.egress_bytes_total), same
  // pattern the global "Bandwidth out" tile already gets from Prometheus.
  const [streamBandwidth, setStreamBandwidth] = useState<Record<string, number>>({});
  const bandwidthSamplesRef = useRef(new Map<string, { bytes: number; time: number }>());
  // Source-transcode jobs across every application, fetched for the homepage
  // "Top links" table. Per-application tabs still fetch these on demand
  // (ApplicationDetailPage.loadSourceJobs); this is a separate copy kept in
  // sync on the same 5s poll as the rest of the home dashboard.
  const [sourceJobs, setSourceJobs] = useState<SourceTranscodeJob[]>([]);
  // Live bitrate per source-transcode job, derived the same way as
  // streamBandwidth (delta of a cumulative byte counter between polls).
  const [sourceBandwidth, setSourceBandwidth] = useState<Record<string, number>>({});

  const refreshTemplates = useCallback(async () => {
    const records = await client.listTemplates();
    setTemplates(templatesFromRecords(records));
  }, [client]);

  useEffect(() => {
    refreshTemplates().catch((error) => {
      setNotice({ type: "error", message: error instanceof Error ? error.message : "Could not load transcoding templates." });
    });
  }, [refreshTemplates]);

  const setBandwidth = (value: number) => {
    setBandwidthState(value);
    localStorage.setItem("streamforge-bandwidth", String(value));
  };

  // Guards against two polls overlapping if a request is slow (a stalled
  // connection shouldn't pile up parallel fetches once it finally resolves).
  const refreshInFlightRef = useRef(false);

  const refresh = useCallback(async (activeClient = client, quiet = false) => {
    if (refreshInFlightRef.current) return;
    refreshInFlightRef.current = true;
    if (!quiet) setLoading(true);
    try {
      const next = await activeClient.snapshot();
      setSnapshot(next);
      setHistory((current) => [...current, metric(next, "active_viewers")].slice(-24));

      const now = Date.now();
      const samples = bandwidthSamplesRef.current;
      const nextBandwidth: Record<string, number> = {};
      for (const stream of next.streams) {
        const key = `${stream.application}:${stream.name}`;
        const bytes = stream.egress_bytes_total;
        if (bytes === undefined) continue;
        const previous = samples.get(key);
        // Only trust a delta between two samples of a still-live stream: a
        // restart resets the counter, and negative/zero elapsed time can't
        // give a meaningful rate.
        if (previous && bytes >= previous.bytes && now > previous.time) {
          nextBandwidth[key] = ((bytes - previous.bytes) * 8) / ((now - previous.time) / 1000);
        }
        samples.set(key, { bytes, time: now });
      }
      setStreamBandwidth(nextBandwidth);

      try {
        const jobs = (
          await Promise.all(next.applications.map((app) => activeClient.listSourceTranscodes(app.name)))
        ).flat();
        setSourceJobs(jobs);

        // Same cumulative-bytes-delta derivation as streams above, just under
        // a "job:" key prefix so a job can't collide with a plain stream of
        // the same name in the shared samples map.
        const nextSourceBandwidth: Record<string, number> = {};
        for (const job of jobs) {
          if (job.bytes_total === undefined) continue;
          const key = `job:${job.application}:${job.name}`;
          const previous = samples.get(key);
          if (previous && job.bytes_total >= previous.bytes && now > previous.time) {
            nextSourceBandwidth[key] = ((job.bytes_total - previous.bytes) * 8) / ((now - previous.time) / 1000);
          }
          samples.set(key, { bytes: job.bytes_total, time: now });
        }
        setSourceBandwidth(nextSourceBandwidth);
      } catch {
        // Homepage source-job row list degrades gracefully; the per-application
        // Source Transcode tab surfaces the real error when it fetches directly.
      }
    } catch (error) {
      if (!quiet) {
        setNotice({ type: "error", message: error instanceof Error ? error.message : "Could not refresh server data." });
      }
    } finally {
      refreshInFlightRef.current = false;
      if (!quiet) setLoading(false);
    }
  }, [client]);

  // Poll every 5s, but only while the tab is actually visible and online.
  // A background/minimized tab still burns the user's data and the server's
  // request budget for a dashboard nobody is looking at, and some browsers
  // throttle/queue timers in hidden tabs anyway, which can make requests
  // pile up. Refresh immediately on return instead, so the numbers are never
  // stale when the operator tabs back in.
  useEffect(() => {
    refresh(client);
    const interval = window.setInterval(() => {
      if (document.hidden || !navigator.onLine) return;
      refresh(client, true);
    }, 5000);
    const onVisibilityOrOnline = () => {
      if (document.hidden || !navigator.onLine) return;
      refresh(client, true);
    };
    document.addEventListener("visibilitychange", onVisibilityOrOnline);
    window.addEventListener("online", onVisibilityOrOnline);
    return () => {
      window.clearInterval(interval);
      document.removeEventListener("visibilitychange", onVisibilityOrOnline);
      window.removeEventListener("online", onVisibilityOrOnline);
    };
  }, [client, refresh]);

  useEffect(() => {
    if (!notice) return;
    const timeout = window.setTimeout(() => setNotice(null), 4500);
    return () => window.clearTimeout(timeout);
  }, [notice]);

  useEffect(() => {
    let cancelled = false;
    fetch("/runtime-config.json", { cache: "no-store" })
      .then((response) => {
        if (!response.ok) throw new Error("No server runtime config.");
        return response.json() as Promise<{ bandwidth_mbps?: number }>;
      })
      .then((runtime) => {
        if (cancelled || !Number.isFinite(runtime.bandwidth_mbps) || (runtime.bandwidth_mbps ?? 0) < 10) return;
        setBandwidth(runtime.bandwidth_mbps as number);
      })
      .catch(() => {
        // Development and demo mode intentionally have no installer-owned
        // runtime file; retain the operator's local planner value.
      });
    return () => {
      cancelled = true;
    };
  }, []);

  const notify = useCallback((type: "success" | "error", message: string) => setNotice({ type, message }), []);
  const perform = async (work: () => Promise<unknown>, message: string) => {
    try {
      await work();
      notify("success", message);
      await refresh();
    } catch (error) {
      notify("error", error instanceof Error ? error.message : "The action failed.");
    }
  };

  const streamAction = (stream: Stream, action: string) => {
    if (action === "toggle") {
      perform(() => client.patchStream(stream, { enabled: !stream.enabled }), `Stream ${stream.enabled ? "disabled" : "enabled"}.`);
    } else {
      setActionStream(stream);
    }
  };

  const viewers = metric(snapshot, "active_viewers");
  const publishers = metric(snapshot, "active_publishers");
  const currentIngress = metric(snapshot, "ingress_bitrate");
  const currentEgress = metric(snapshot, "egress_bitrate");
  const measuredBitrateMbps = viewers > 0 && currentEgress > 0
    ? currentEgress / viewers / 1e6
    : publishers > 0 && currentIngress > 0
      ? currentIngress / publishers / 1e6
      : 0;
  const measuredBitrateSource = viewers > 0 && currentEgress > 0
    ? "Measured from delivered egress per active viewer."
    : "Measured from OBS/transcoder ingress per active publisher.";
  const title = page === "applications" && selectedApplication
    ? { title: selectedApplication.name, subtitle: "Application configuration and live activity" }
    : pageTitles[page];
  const navigate = (nextPage: Page) => {
    setPage(nextPage);
    if (nextPage === "applications") {
      setSelectedApplication(null);
      setApplicationTab("playback");
    }
  };

  return (
    <div className="app-shell">
      <Sidebar page={page} setPage={navigate} open={mobileNav} close={() => setMobileNav(false)} />
      <main className="main-shell">
        <header className="topbar">
          <button className="menu-button" aria-label="Open navigation" onClick={() => setMobileNav(true)}><Menu size={21} /></button>
          <div className="page-title"><h1>{title.title}</h1><p>{title.subtitle}</p></div>
          <div className="topbar-actions">
            {demo && <span className="demo-badge"><SquareActivity size={14} /> Demo data</span>}
            <div className="node-status"><span className="pulse" /><div><strong>Origin node</strong><small>{snapshot.health === "online" ? "Online" : "Connecting…"}</small></div></div>
            <IconButton label="Refresh data" onClick={() => refresh()}><RefreshCw className={loading ? "spin" : ""} size={17} /></IconButton>
          </div>
        </header>
        <div className="content">
          {page === "home" && <Overview snapshot={snapshot} history={history} bandwidth={bandwidth} streamBandwidth={streamBandwidth} sourceJobs={sourceJobs} sourceBandwidth={sourceBandwidth} onNavigate={navigate} onStreamAction={streamAction} />}
          {page === "applications" && (
            selectedApplication
              ? <ApplicationDetailPage
                  application={selectedApplication}
                  streams={snapshot.streams}
                  templates={templates}
                  client={client}
                  activeTab={applicationTab}
                  setActiveTab={setApplicationTab}
                  onNotify={notify}
                  onDeleteRequest={setPendingDeleteApplication}
                  onBack={() => {
                    setSelectedApplication(null);
                    setApplicationTab("playback");
                  }}
                />
              : <ApplicationsPage
                  applications={snapshot.applications}
                  streams={snapshot.streams}
                  openCreate={() => setCreateType("application")}
                  onOpen={(application) => {
                    setSelectedApplication(application);
                    setApplicationTab("playback");
                  }}
                />
          )}
          {page === "transcode" && (
            <TranscodePage templates={templates} client={client} onNotify={notify} refreshTemplates={refreshTemplates} />
          )}
          {page === "server" && (
            <div className="workspace-stack">
              <SystemPage snapshot={snapshot} />
              <div className="workspace-divider"><span>CAPACITY</span><strong>Delivery planner</strong><p>Estimate safe direct-origin viewer capacity from the available uplink.</p></div>
              <CapacityPage
                bandwidth={bandwidth}
                setBandwidth={setBandwidth}
                currentEgress={currentEgress}
                viewers={viewers}
                measuredBitrateMbps={measuredBitrateMbps}
                measuredBitrateSource={measuredBitrateSource}
              />
            </div>
          )}
        </div>
      </main>

      {notice && <div className={`toast ${notice.type}`}>{notice.type === "success" ? <Check size={18} /> : <X size={18} />}<span>{notice.message}</span></div>}

      {createType === "application" && (
        <CreateApplicationModal
          onClose={() => setCreateType(null)}
          onSubmit={(name) => perform(async () => { await client.createApplication(name); setCreateType(null); }, "Application created.")}
        />
      )}
      {actionStream && (
        <Modal title={actionStream.name} description={`${actionStream.application} / ${actionStream.name}`} onClose={() => setActionStream(null)}>
          <div className="action-menu">
            <button onClick={() => {
              const stream = actionStream;
              setActionStream(null);
              perform(() => client.patchStream(stream, { recording_enabled: !stream.recording_enabled }), `Recording policy ${stream.recording_enabled ? "disabled" : "requested"}.`);
            }}><span><FileVideo size={18} /></span><div><strong>{actionStream.recording_enabled ? "Disable" : "Request"} recording policy</strong><small>Persist the desired policy; runtime recording must also be enabled on the server</small></div><ChevronRight size={17} /></button>
            <button className="danger-action" onClick={() => {
              setPendingDelete(actionStream);
              setActionStream(null);
            }}><span><Trash2 size={18} /></span><div><strong>Delete stream permanently</strong><small>Disconnect its publisher and viewers, then remove it from the server</small></div><ChevronRight size={17} /></button>
          </div>
        </Modal>
      )}
      {pendingDelete && (
        <Modal title={`Delete ${pendingDelete.name}?`} description={`${pendingDelete.application} / ${pendingDelete.name}`} onClose={() => setPendingDelete(null)}>
          <div className="delete-warning">
            <Trash2 size={21} />
            <div><strong>This cannot be undone.</strong><span>The RTMP link will stop working immediately and any connected publisher or viewers will be disconnected.</span></div>
          </div>
          <div className="modal-actions">
            <button className="secondary-button" onClick={() => setPendingDelete(null)}>Cancel</button>
            <button className="danger-button" onClick={() => {
              const stream = pendingDelete;
              setPendingDelete(null);
              perform(() => client.deleteStream(stream), "Stream deleted.");
            }}><Trash2 size={17} /> Delete stream</button>
          </div>
        </Modal>
      )}
      {pendingDeleteApplication && (
        <Modal
          title={`Delete ${pendingDeleteApplication.name}?`}
          description={`${snapshot.streams.filter((stream) => stream.application === pendingDeleteApplication.name).length} stream(s) in this application`}
          onClose={() => setPendingDeleteApplication(null)}
        >
          <div className="delete-warning">
            <Trash2 size={21} />
            <div><strong>This cannot be undone.</strong><span>Every stream and playback link under this application will be deleted along with it.</span></div>
          </div>
          <div className="modal-actions">
            <button className="secondary-button" onClick={() => setPendingDeleteApplication(null)}>Cancel</button>
            <button className="danger-button" onClick={() => {
              const application = pendingDeleteApplication;
              setPendingDeleteApplication(null);
              setSelectedApplication(null);
              setApplicationTab("playback");
              perform(() => client.deleteApplication(application.name), "Application and its streams deleted.");
            }}><Trash2 size={17} /> Delete application</button>
          </div>
        </Modal>
      )}
    </div>
  );
}

function CreateApplicationModal({ onClose, onSubmit }: { onClose: () => void; onSubmit: (name: string) => void }) {
  const [name, setName] = useState("");
  return (
    <Modal title="Create application" description="Add a namespace for one or more related RTMP streams." onClose={onClose}>
      <form className="modal-form" onSubmit={(event: FormEvent) => { event.preventDefault(); onSubmit(name.trim()); }}>
        <label htmlFor="application-name">Application name<input id="application-name" autoFocus required pattern="[A-Za-z0-9_-]+" value={name} onChange={(event) => setName(event.target.value)} placeholder="for example: live" /><small>Letters, numbers, underscores and hyphens only.</small></label>
        <div className="modal-actions"><button type="button" className="secondary-button" onClick={onClose}>Cancel</button><button className="primary-button" disabled={!name.trim()}><Plus size={17} /> Create application</button></div>
      </form>
    </Modal>
  );
}

export default App;
