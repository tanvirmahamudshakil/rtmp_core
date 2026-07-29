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
import { FormEvent, ReactNode, useCallback, useEffect, useState } from "react";
import { Application, ControlClient, Snapshot, Stream } from "./api";

type Page = "home" | "applications" | "transcode" | "server";
type ApplicationTab = "playback" | "transcoding";
type VideoCodec = "H.263" | "H.264" | "H.265" | "VP8" | "VP9" | "Passthrough" | "Disabled";
type EncodingImplementation = "Automatic" | "Software" | "Hardware GPU";
type EncodingPreset = {
  id: string;
  name: string;
  outgoingStreamName: string;
  description: string;
  videoCodec: VideoCodec;
  videoBitrate: number;
  implementation: EncodingImplementation;
  gpuMode: "first" | "specific";
  gpuId: number | null;
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
  { id: "transcoding", label: "Transcoding" }
];
const videoCodecs: VideoCodec[] = ["H.263", "H.264", "H.265", "VP8", "VP9", "Passthrough", "Disabled"];
const transcodingStorageKey = "streamforge-transcoding-templates";
const newLocalId = () => globalThis.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
const loadTranscodingTemplates = (): TranscodingTemplate[] => {
  try {
    const stored = JSON.parse(localStorage.getItem(transcodingStorageKey) ?? "[]");
    return Array.isArray(stored) ? stored : [];
  } catch {
    return [];
  }
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
  onNavigate,
  onStreamAction
}: {
  snapshot: Snapshot;
  history: number[];
  bandwidth: number;
  onNavigate: (page: Page) => void;
  onStreamAction: (stream: Stream, action: string) => void;
}) {
  const viewers = metric(snapshot, "active_viewers");
  const publishers = metric(snapshot, "active_publishers");
  const egress = metric(snapshot, "egress_bitrate");
  const utilization = Math.min((egress / (bandwidth * 1e6)) * 100, 999);
  const liveStreams = snapshot.streams.filter((stream) => stream.is_live);
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
          value={`${(metric(snapshot, "worker_cpu_usage") / 1000).toFixed(1)} cores`}
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
        <StreamTable streams={(liveStreams.length ? liveStreams : snapshot.streams).slice(0, 5)} onAction={onStreamAction} compactMode />
      </section>
    </>
  );
}

function StreamTable({
  streams,
  onAction,
  compactMode = false
}: {
  streams: Stream[];
  onAction: (stream: Stream, action: string) => void;
  compactMode?: boolean;
}) {
  if (!streams.length) {
    return <div className="empty-state"><Radio size={28} /><strong>No streams available</strong><span>Streams matching this view will appear here automatically.</span></div>;
  }
  return (
    <div className="table-wrap">
      <table>
        <thead><tr><th>Stream</th><th>Status</th><th>Viewers</th><th>Recording</th><th>Delivery</th><th><span className="sr-only">Actions</span></th></tr></thead>
        <tbody>
          {streams.map((stream) => (
            <tr key={`${stream.application}:${stream.name}`}>
              <td>
                <div className="stream-identity"><span className="stream-avatar"><Video size={17} /></span><div><strong>{stream.name}</strong><small>{stream.application} / {stream.name}</small></div></div>
              </td>
              <td><StatusPill live={stream.is_live} /></td>
              <td><strong className="viewer-count">{stream.viewer_count === undefined ? "—" : compact(stream.viewer_count)}</strong></td>
              <td><span className={`recording-state ${stream.recording_enabled ? "on" : ""}`}><span />{stream.recording_enabled ? "Requested" : "Off"}</span></td>
              <td><span className={stream.enabled ? "enabled-text" : "disabled-text"}>{stream.enabled ? "Enabled" : "Disabled"}</span></td>
              <td>
                <div className="row-actions">
                  <IconButton label="Copy universal RTMP URL" onClick={() => copyText(stream.rtmp_url)}><Copy size={16} /></IconButton>
                  <IconButton label="Copy smooth HLS playback URL" onClick={() => copyText(absoluteUrl(stream.hls_path))}><ArrowDownToLine size={16} /></IconButton>
                  {!compactMode && <IconButton label={stream.enabled ? "Disable stream" : "Enable stream"} onClick={() => onAction(stream, "toggle")}><SlidersHorizontal size={16} /></IconButton>}
                  <IconButton label="More stream actions" onClick={() => onAction(stream, "more")}><MoreHorizontal size={17} /></IconButton>
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
  activeTab,
  setActiveTab,
  onBack
}: {
  application: Application;
  streams: Stream[];
  activeTab: ApplicationTab;
  setActiveTab: (tab: ApplicationTab) => void;
  onBack: () => void;
}) {
  const applicationStreams = streams.filter((stream) => stream.application === application.name);
  const [copiedUrl, setCopiedUrl] = useState<string | null>(null);
  const copyUrl = async (url: string) => {
    await copyText(url);
    setCopiedUrl(url);
    window.setTimeout(() => setCopiedUrl((current) => current === url ? null : current), 1500);
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
    return (
      <section className="application-feature-placeholder">
        <span className="feature-icon"><Workflow size={24} /></span>
        <span className="placeholder-badge">TRANSCODING</span>
        <h2>Transcoding workspace</h2>
        <p>This tab is ready for the transcoding profiles and controls you will provide later.</p>
        <small>{application.name} / transcoding</small>
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

function NewTemplateModal({ onClose, onAdd }: { onClose: () => void; onAdd: (name: string) => void }) {
  const [name, setName] = useState("");
  return (
    <Modal title="Add new template" description="Give this transcoding template a clear name." onClose={onClose}>
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
          <button className="primary-button" disabled={!name.trim()}><Plus size={17} /> Add template</button>
        </div>
      </form>
    </Modal>
  );
}

function NewPresetModal({ onClose, onAdd }: { onClose: () => void; onAdd: (preset: EncodingPreset) => void }) {
  const [name, setName] = useState("");
  const [outgoingStreamName, setOutgoingStreamName] = useState("");
  const [description, setDescription] = useState("");
  const [videoCodec, setVideoCodec] = useState<VideoCodec>("H.264");
  const [videoBitrate, setVideoBitrate] = useState("2500000");
  const [implementation, setImplementation] = useState<EncodingImplementation>("Automatic");
  const [gpuMode, setGpuMode] = useState<"first" | "specific">("first");
  const [gpuId, setGpuId] = useState("0");
  return (
    <Modal title="Add encoding preset" description="Configure the outgoing video encoding profile." onClose={onClose} wide>
      <form className="modal-form preset-form" onSubmit={(event: FormEvent) => {
        event.preventDefault();
        onAdd({
          id: newLocalId(),
          name: name.trim(),
          outgoingStreamName: outgoingStreamName.trim(),
          description: description.trim(),
          videoCodec,
          videoBitrate: Math.max(0, Number(videoBitrate) || 0),
          implementation,
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
              {videoCodecs.map((codec) => <option key={codec}>{codec}</option>)}
            </select>
          </label>
          <label htmlFor="video-bitrate">Video Bitrate
            <div className="field-with-unit">
              <input id="video-bitrate" type="number" min="0" step="1000" value={videoBitrate} onChange={(event) => setVideoBitrate(event.target.value)} />
              <span>bps</span>
            </div>
            <small>Bits per second (bps)</small>
          </label>
          <label htmlFor="encoding-implementation">Encoding Implementation
            <select id="encoding-implementation" value={implementation} onChange={(event) => setImplementation(event.target.value as EncodingImplementation)}>
              <option>Automatic</option>
              <option>Software</option>
              <option>Hardware GPU</option>
            </select>
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

        <div className="modal-actions">
          <button type="button" className="secondary-button" onClick={onClose}>Cancel</button>
          <button className="primary-button" disabled={!name.trim() || !outgoingStreamName.trim()}><Plus size={17} /> Add preset</button>
        </div>
      </form>
    </Modal>
  );
}

function TranscodePage() {
  const [templates, setTemplates] = useState<TranscodingTemplate[]>(loadTranscodingTemplates);
  const [selectedTemplateId, setSelectedTemplateId] = useState<string | null>(null);
  const [showTemplateModal, setShowTemplateModal] = useState(false);
  const [showPresetModal, setShowPresetModal] = useState(false);
  const selectedTemplate = templates.find((template) => template.id === selectedTemplateId) ?? null;

  useEffect(() => {
    localStorage.setItem(transcodingStorageKey, JSON.stringify(templates));
  }, [templates]);

  const addTemplate = (name: string) => {
    const template: TranscodingTemplate = { id: newLocalId(), name, presets: [] };
    setTemplates((current) => [...current, template]);
    setShowTemplateModal(false);
  };
  const addPreset = (preset: EncodingPreset) => {
    if (!selectedTemplateId) return;
    setTemplates((current) => current.map((template) =>
      template.id === selectedTemplateId ? { ...template, presets: [...template.presets, preset] } : template
    ));
    setShowPresetModal(false);
  };

  if (selectedTemplate) {
    return (
      <div className="template-detail">
        <section className="template-detail-header">
          <button className="back-button" type="button" onClick={() => setSelectedTemplateId(null)}><ArrowLeft size={17} /> Transcoding templates</button>
          <div className="template-title-row">
            <span className="template-large-icon"><Workflow size={24} /></span>
            <div><span className="eyebrow">TRANSCODING TEMPLATE</span><h2>{selectedTemplate.name}</h2><p>{selectedTemplate.presets.length} encoding presets configured</p></div>
          </div>
          <div className="template-tabs"><button className="active" type="button">Encoding Presets</button></div>
        </section>
        <section className="preset-workspace">
          <div className="preset-workspace-heading">
            <div><span className="eyebrow">OUTPUT PROFILES</span><h2>Encoding Presets</h2><p>Create one preset for every outgoing stream rendition.</p></div>
            <button className="primary-button" onClick={() => setShowPresetModal(true)}><Plus size={17} /> Add Preset</button>
          </div>
          <div className="preset-grid">
            {selectedTemplate.presets.map((preset) => (
              <article className="preset-card" key={preset.id}>
                <div className="preset-card-head"><span><Video size={18} /></span><div><strong>{preset.name}</strong><small>{preset.outgoingStreamName}</small></div><b>{preset.videoCodec}</b></div>
                {preset.description && <p>{preset.description}</p>}
                <div className="preset-card-stats">
                  <div><span>Video bitrate</span><strong>{preset.videoBitrate ? `${preset.videoBitrate.toLocaleString()} bps` : "Automatic"}</strong></div>
                  <div><span>Implementation</span><strong>{preset.implementation}</strong></div>
                  <div><span>GPU</span><strong>{preset.gpuMode === "first" ? "First available" : `GPU ${preset.gpuId}`}</strong></div>
                </div>
              </article>
            ))}
            {!selectedTemplate.presets.length && (
              <div className="empty-state full preset-empty"><SlidersHorizontal size={30} /><strong>No encoding presets</strong><span>Add the first output profile for this template.</span></div>
            )}
          </div>
        </section>
        {showPresetModal && <NewPresetModal onClose={() => setShowPresetModal(false)} onAdd={addPreset} />}
      </div>
    );
  }

  return (
    <div className="transcode-template-page">
      <div className="section-intro">
        <div><span className="eyebrow">TRANSCODING</span><h2>Templates</h2><p>Create reusable encoding presets for your outgoing streams.</p></div>
        <button className="primary-button" onClick={() => setShowTemplateModal(true)}><Plus size={17} /> Add New Template</button>
      </div>
      <section className="template-grid">
        {templates.map((template, index) => (
          <button className="template-card" type="button" key={template.id} onClick={() => setSelectedTemplateId(template.id)}>
            <div className={`template-icon tone-${index % 4}`}><Workflow size={22} /></div>
            <span className="template-open"><ChevronRight size={17} /></span>
            <span className="template-label">Template {String(index + 1).padStart(2, "0")}</span>
            <h3>{template.name}</h3>
            <p>{template.presets.length ? `${template.presets.length} encoding presets` : "No presets configured yet"}</p>
            <div className="template-card-foot"><span><Cpu size={14} /> Encoding template</span><b>{template.presets.length}</b></div>
          </button>
        ))}
        {!templates.length && (
          <div className="empty-state full template-empty"><Workflow size={31} /><strong>No transcoding templates</strong><span>Create a template, then add one or more encoding presets.</span><button className="primary-button" onClick={() => setShowTemplateModal(true)}><Plus size={17} /> Add New Template</button></div>
        )}
      </section>
      {showTemplateModal && <NewTemplateModal onClose={() => setShowTemplateModal(false)} onAdd={addTemplate} />}
      <div className="storage-note"><Database size={16} /><span>Template drafts are saved in this browser until the transcoding server API is connected.</span></div>
    </div>
  );
}

function App() {
  const demo = new URLSearchParams(window.location.search).get("demo") === "1";
  const [client] = useState(() => new ControlClient(demo));
  const [snapshot, setSnapshot] = useState<Snapshot>(EMPTY_SNAPSHOT);
  const [page, setPage] = useState<Page>("home");
  const [selectedApplication, setSelectedApplication] = useState<Application | null>(null);
  const [applicationTab, setApplicationTab] = useState<ApplicationTab>("playback");
  const [loading, setLoading] = useState(false);
  const [notice, setNotice] = useState<Notice>(null);
  const [mobileNav, setMobileNav] = useState(false);
  const [createType, setCreateType] = useState<"application" | null>(null);
  const [actionStream, setActionStream] = useState<Stream | null>(null);
  const [pendingDelete, setPendingDelete] = useState<Stream | null>(null);
  const [bandwidth, setBandwidthState] = useState(() => Number(localStorage.getItem("streamforge-bandwidth")) || 10000);
  const [history, setHistory] = useState<number[]>(demo ? [5200, 5400, 5310, 5710, 5890, 6030, 6210, 6150, 6420, 6615] : []);

  const setBandwidth = (value: number) => {
    setBandwidthState(value);
    localStorage.setItem("streamforge-bandwidth", String(value));
  };

  const refresh = useCallback(async (activeClient = client, quiet = false) => {
    if (!quiet) setLoading(true);
    try {
      const next = await activeClient.snapshot();
      setSnapshot(next);
      setHistory((current) => [...current, metric(next, "active_viewers")].slice(-24));
    } catch (error) {
      if (!quiet) {
        setNotice({ type: "error", message: error instanceof Error ? error.message : "Could not refresh server data." });
      }
    } finally {
      if (!quiet) setLoading(false);
    }
  }, [client]);

  useEffect(() => {
    refresh(client);
    const interval = window.setInterval(() => refresh(client, true), 5000);
    return () => window.clearInterval(interval);
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

  const notify = (type: "success" | "error", message: string) => setNotice({ type, message });
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
          {page === "home" && <Overview snapshot={snapshot} history={history} bandwidth={bandwidth} onNavigate={navigate} onStreamAction={streamAction} />}
          {page === "applications" && (
            selectedApplication
              ? <ApplicationDetailPage
                  application={selectedApplication}
                  streams={snapshot.streams}
                  activeTab={applicationTab}
                  setActiveTab={setApplicationTab}
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
          {page === "transcode" && <TranscodePage />}
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
