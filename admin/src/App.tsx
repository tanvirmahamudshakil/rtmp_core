import {
  Activity,
  AppWindow,
  ArrowDownToLine,
  ArrowUpRight,
  BarChart3,
  Check,
  ChevronRight,
  CircleGauge,
  Clipboard,
  CloudOff,
  Copy,
  Cpu,
  Database,
  Eye,
  EyeOff,
  FileVideo,
  Gauge,
  HardDrive,
  KeyRound,
  Layers3,
  LockKeyhole,
  LogOut,
  Menu,
  MoreHorizontal,
  Network,
  Plus,
  Radio,
  RadioTower,
  RefreshCw,
  Router,
  Search,
  Server,
  Settings2,
  ShieldCheck,
  Signal,
  SlidersHorizontal,
  SquareActivity,
  Unplug,
  Users,
  Video,
  X,
  Zap
} from "lucide-react";
import { FormEvent, ReactNode, useCallback, useEffect, useMemo, useState } from "react";
import { ApiError, Application, ControlClient, Snapshot, Stream, StreamSecret } from "./api";

type Page = "overview" | "streams" | "applications" | "capacity" | "system";
type Notice = { type: "success" | "error"; message: string } | null;

const EMPTY_SNAPSHOT: Snapshot = { applications: [], streams: [], metrics: {}, health: "offline" };
const pageTitles: Record<Page, { title: string; subtitle: string }> = {
  overview: { title: "Overview", subtitle: "Your live service at a glance" },
  streams: { title: "Streams", subtitle: "Create, monitor and secure every stream" },
  applications: { title: "Applications", subtitle: "Organize streams into simple namespaces" },
  capacity: { title: "Capacity", subtitle: "Plan direct delivery without a CDN" },
  system: { title: "System", subtitle: "Server health and resource usage" }
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
            <span className="eyebrow">SECURE CONTROL</span>
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

function CopyField({ label, value, secret = false }: { label: string; value: string; secret?: boolean }) {
  const [visible, setVisible] = useState(!secret);
  const [copied, setCopied] = useState(false);
  const copy = async () => {
    try {
      if (navigator.clipboard && window.isSecureContext) {
        await navigator.clipboard.writeText(value);
      } else {
        const fallback = document.createElement("textarea");
        fallback.value = value;
        fallback.setAttribute("readonly", "");
        fallback.style.position = "fixed";
        fallback.style.opacity = "0";
        document.body.appendChild(fallback);
        fallback.select();
        const copied = document.execCommand("copy");
        fallback.remove();
        if (!copied) throw new Error("Clipboard access is unavailable.");
      }
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1500);
    } catch {
      // Plain HTTP and strict browser policies can deny the Clipboard API.
      // Reveal the selectable value so the operator can still copy it.
      setVisible(true);
    }
  };
  return (
    <div className="copy-block">
      <div className="copy-label">
        <span>{label}</span>
        {secret && (
          <button type="button" className="text-action" onClick={() => setVisible((current) => !current)}>
            {visible ? <EyeOff size={14} /> : <Eye size={14} />} {visible ? "Hide" : "Reveal"}
          </button>
        )}
      </div>
      <div className="copy-field">
        <code>{visible ? value : "••••••••••••••••••••••••••••••••"}</code>
        <IconButton label={`Copy ${label}`} onClick={copy}>
          {copied ? <Check size={17} /> : <Copy size={17} />}
        </IconButton>
      </div>
    </div>
  );
}

function Login({
  onLogin,
  loading,
  error,
  demo
}: {
  onLogin: (token: string) => void;
  loading: boolean;
  error: string;
  demo: boolean;
}) {
  const [token, setToken] = useState(demo ? "demo-session" : "");
  const [show, setShow] = useState(false);
  return (
    <main className="login-shell">
      <div className="login-grid" aria-hidden="true" />
      <section className="login-brand">
        <div className="brand-lockup large">
          <div className="brand-mark">
            <RadioTower size={25} />
          </div>
          <div>
            <strong>StreamForge</strong>
            <span>CONTROL PLANE</span>
          </div>
        </div>
        <div className="login-statement">
          <span className="eyebrow">STREAMFORGE CONTROL</span>
          <h1>Live streaming,<br />simply managed.</h1>
          <p>
            Publish securely, watch audience growth and keep your origin healthy
            from one focused dashboard.
          </p>
        </div>
        <div className="login-points">
          <span><ShieldCheck size={16} /> Secure by default</span>
          <span><Network size={16} /> Clear bandwidth visibility</span>
        </div>
      </section>
      <section className="login-panel">
        <form
          className="login-card"
          onSubmit={(event) => {
            event.preventDefault();
            onLogin(token.trim());
          }}
        >
          <div className="login-card-icon"><LockKeyhole size={22} /></div>
          <span className="eyebrow">WELCOME BACK</span>
          <h2>Sign in to StreamForge</h2>
          <p>Enter the admin token created during server setup.</p>
          <label htmlFor="admin-token">Admin bearer token</label>
          <div className="password-field">
            <KeyRound size={18} />
            <input
              id="admin-token"
              autoFocus
              autoComplete="current-password"
              type={show ? "text" : "password"}
              value={token}
              onChange={(event) => setToken(event.target.value)}
              placeholder="Paste your 64-character token"
            />
            <button type="button" title={show ? "Hide token" : "Show token"} onClick={() => setShow((value) => !value)}>
              {show ? <EyeOff size={17} /> : <Eye size={17} />}
            </button>
          </div>
          {error && <div className="form-error">{error}</div>}
          {demo && <div className="demo-note"><SquareActivity size={16} /> Preview mode is using safe demo data.</div>}
          <button className="primary-button login-button" disabled={!token.trim() || loading}>
            {loading ? <RefreshCw className="spin" size={17} /> : <ArrowUpRight size={17} />}
            {loading ? "Checking access…" : "Continue to dashboard"}
          </button>
          <small>Your token stays in this browser tab and is never bundled with the app.</small>
        </form>
      </section>
    </main>
  );
}

function Sidebar({
  page,
  setPage,
  onLogout,
  open,
  close
}: {
  page: Page;
  setPage: (page: Page) => void;
  onLogout: () => void;
  open: boolean;
  close: () => void;
}) {
  const nav: { page: Page; label: string; icon: ReactNode }[] = [
    { page: "overview", label: "Overview", icon: <BarChart3 size={18} /> },
    { page: "streams", label: "Streams", icon: <Radio size={18} /> },
    { page: "applications", label: "Applications", icon: <Layers3 size={18} /> },
    { page: "capacity", label: "Capacity planner", icon: <Gauge size={18} /> },
    { page: "system", label: "System", icon: <Server size={18} /> }
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
          <div className="edge-icon"><CloudOff size={18} /></div>
          <div><strong>Direct origin</strong><span>Serving viewers without a CDN</span></div>
          <StatusPill live label="Ready" />
        </div>
        <button className="logout-button" onClick={onLogout}>
          <LogOut size={17} /> Sign out
        </button>
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
            <button className="subtle-button" onClick={() => onNavigate("capacity")}>
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
            <div><span className="health-icon good"><ShieldCheck size={18} /></span><p><strong>Authentication</strong><small>{compact(metric(snapshot, "authentication_failures"))} rejected attempts</small></p><b>Protected</b></div>
            <div><span className="health-icon good"><Database size={18} /></span><p><strong>SQLite control store</strong><small>Readiness check passed</small></p><b>Ready</b></div>
            <div><span className="health-icon good"><HardDrive size={18} /></span><p><strong>Outbound queue</strong><small>{bytes(metric(snapshot, "outbound_queue_bytes"))} pending</small></p><b>Stable</b></div>
            <div><span className={`health-icon ${metric(snapshot, "slow_viewer_evictions") > 10 ? "warn" : "good"}`}><Unplug size={18} /></span><p><strong>Slow viewers</strong><small>Automatic backpressure policy</small></p><b>{compact(metric(snapshot, "slow_viewer_evictions"))} evicted</b></div>
          </div>
        </article>
      </section>

      <section className="panel">
        <div className="panel-heading">
          <div><span className="eyebrow">LIVE NOW</span><h2>Top streams</h2></div>
          <button className="subtle-button" onClick={() => onNavigate("streams")}>View all streams <ChevronRight size={16} /></button>
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
    return <div className="empty-state"><Radio size={28} /><strong>No streams yet</strong><span>Create a stream to receive a secure publish key.</span></div>;
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
                  {stream.playback_url && <IconButton label="Copy playback URL" onClick={() => copyText(stream.playback_url!)}><Copy size={16} /></IconButton>}
                  {!compactMode && <IconButton label="Create playback token" onClick={() => onAction(stream, "token")}><KeyRound size={16} /></IconButton>}
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

function StreamsPage({
  streams,
  onAction,
  openCreate
}: {
  streams: Stream[];
  onAction: (stream: Stream, action: string) => void;
  openCreate: () => void;
}) {
  const [query, setQuery] = useState("");
  const [filter, setFilter] = useState<"all" | "live" | "idle">("all");
  const shown = streams.filter((stream) => {
    const matches = `${stream.application} ${stream.name}`.toLowerCase().includes(query.toLowerCase());
    return matches && (filter === "all" || (filter === "live" ? stream.is_live : !stream.is_live));
  });
  return (
    <section className="panel">
      <div className="toolbar">
        <div className="search-field"><Search size={17} /><input value={query} onChange={(event) => setQuery(event.target.value)} placeholder="Search streams…" /></div>
        <div className="segmented" role="group" aria-label="Filter streams">
          {(["all", "live", "idle"] as const).map((option) => (
            <button key={option} className={filter === option ? "active" : ""} onClick={() => setFilter(option)}>
              {option === "all" ? `All ${streams.length}` : option === "live" ? "Live now" : "Idle"}
            </button>
          ))}
        </div>
        <button className="primary-button" onClick={openCreate}><Plus size={17} /> New stream</button>
      </div>
      <StreamTable streams={shown} onAction={onAction} />
    </section>
  );
}

function ApplicationsPage({
  applications,
  streams,
  openCreate
}: {
  applications: Application[];
  streams: Stream[];
  openCreate: () => void;
}) {
  return (
    <>
      <div className="section-intro">
        <div><h2>Organize your streams</h2><p>Use applications to group streams by event, product or customer.</p></div>
        <button className="primary-button" onClick={openCreate}><Plus size={17} /> New application</button>
      </div>
      <section className="application-grid">
        {applications.map((app, index) => {
          const appStreams = streams.filter((stream) => stream.application === app.name);
          const live = appStreams.filter((stream) => stream.is_live).length;
          return (
            <article className="application-card" key={app.name}>
              <div className={`app-icon tone-${index % 4}`}><AppWindow size={21} /></div>
              <StatusPill live={app.enabled} label={app.enabled ? "Enabled" : "Disabled"} />
              <h3>{app.name}</h3>
              <code>rtmp://origin:1935/{app.name}</code>
              <div className="app-stats">
                <div><strong>{appStreams.length}</strong><span>Streams</span></div>
                <div><strong>{live}</strong><span>Live now</span></div>
                <div><strong>{compact(appStreams.reduce((sum, stream) => sum + (stream.viewer_count ?? 0), 0))}</strong><span>Viewers</span></div>
              </div>
            </article>
          );
        })}
        {!applications.length && <div className="empty-state full"><Layers3 size={30} /><strong>No applications</strong><span>Create the first namespace before adding a stream.</span></div>}
      </section>
    </>
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
    ["Management API", "Bearer token protected", "Protected", <ShieldCheck size={19} />],
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
            <div><span>Auth rejected</span><strong>{compact(metric(snapshot, "authentication_failures"))}</strong></div>
          </div>
        </section>
      </div>
      <section className="panel config-note">
        <Settings2 size={21} />
        <div><strong>Server-owned configuration</strong><span>Kernel limits, listener queues, worker count and interface shaping are managed by the Linux installer. Secrets stay in <code>/etc/rtmp-server/rtmp-server.env</code> with restricted permissions.</span></div>
      </section>
    </>
  );
}

function App() {
  const demo = new URLSearchParams(window.location.search).get("demo") === "1";
  const initialToken = demo ? "demo-session" : sessionStorage.getItem("streamforge-token") ?? "";
  const [client, setClient] = useState<ControlClient | null>(() =>
    initialToken ? new ControlClient(initialToken, demo) : null
  );
  const [authenticated, setAuthenticated] = useState(Boolean(initialToken));
  const [loginLoading, setLoginLoading] = useState(false);
  const [loginError, setLoginError] = useState("");
  const [snapshot, setSnapshot] = useState<Snapshot>(EMPTY_SNAPSHOT);
  const [page, setPage] = useState<Page>("overview");
  const [loading, setLoading] = useState(false);
  const [notice, setNotice] = useState<Notice>(null);
  const [mobileNav, setMobileNav] = useState(false);
  const [createType, setCreateType] = useState<"stream" | "application" | null>(null);
  const [secret, setSecret] = useState<{ title: string; data: StreamSecret; tokenExpires?: number } | null>(null);
  const [actionStream, setActionStream] = useState<Stream | null>(null);
  const [bandwidth, setBandwidthState] = useState(() => Number(localStorage.getItem("streamforge-bandwidth")) || 10000);
  const [history, setHistory] = useState<number[]>(demo ? [5200, 5400, 5310, 5710, 5890, 6030, 6210, 6150, 6420, 6615] : []);

  const setBandwidth = (value: number) => {
    setBandwidthState(value);
    localStorage.setItem("streamforge-bandwidth", String(value));
  };

  const refresh = useCallback(async (activeClient = client, quiet = false) => {
    if (!activeClient) return;
    if (!quiet) setLoading(true);
    try {
      const next = await activeClient.snapshot();
      setSnapshot(next);
      setHistory((current) => [...current, metric(next, "active_viewers")].slice(-24));
    } catch (error) {
      if (error instanceof ApiError && error.status === 401) {
        sessionStorage.removeItem("streamforge-token");
        setAuthenticated(false);
        setLoginError("Your admin session expired. Sign in again.");
      } else if (!quiet) {
        setNotice({ type: "error", message: error instanceof Error ? error.message : "Could not refresh server data." });
      }
    } finally {
      if (!quiet) setLoading(false);
    }
  }, [client]);

  useEffect(() => {
    if (!authenticated || !client) return;
    refresh(client);
    const interval = window.setInterval(() => refresh(client, true), 5000);
    return () => window.clearInterval(interval);
  }, [authenticated, client, refresh]);

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

  const login = async (nextToken: string) => {
    setLoginLoading(true);
    setLoginError("");
    const nextClient = new ControlClient(nextToken, demo);
    try {
      await nextClient.validate();
      if (!demo) sessionStorage.setItem("streamforge-token", nextToken);
      setClient(nextClient);
      setAuthenticated(true);
    } catch (error) {
      setLoginError(error instanceof ApiError && error.status === 401 ? "That admin token was not accepted." : "The server could not be reached.");
    } finally {
      setLoginLoading(false);
    }
  };

  const logout = () => {
    sessionStorage.removeItem("streamforge-token");
    setClient(null);
    setAuthenticated(false);
    setSnapshot(EMPTY_SNAPSHOT);
  };

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
    if (!client) return;
    if (action === "toggle") {
      perform(() => client.patchStream(stream, { enabled: !stream.enabled }), `Stream ${stream.enabled ? "disabled" : "enabled"}.`);
    } else if (action === "token") {
      perform(async () => {
        const result = await client.playbackToken(stream, 3600);
        setSecret({ title: "Signed playback token", data: { stream_key: result.token }, tokenExpires: result.expires_at });
      }, "Playback token created.");
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
  const title = pageTitles[page];
  const sortedStreams = useMemo(
    () => [...snapshot.streams].sort((a, b) => (b.viewer_count ?? 0) - (a.viewer_count ?? 0)),
    [snapshot.streams]
  );

  if (!authenticated) return <Login onLogin={login} loading={loginLoading} error={loginError} demo={demo} />;

  return (
    <div className="app-shell">
      <Sidebar page={page} setPage={setPage} onLogout={logout} open={mobileNav} close={() => setMobileNav(false)} />
      <main className="main-shell">
        <header className="topbar">
          <button className="menu-button" aria-label="Open navigation" onClick={() => setMobileNav(true)}><Menu size={21} /></button>
          <div className="page-title"><h1>{title.title}</h1><p>{title.subtitle}</p></div>
          <div className="topbar-actions">
            {demo && <span className="demo-badge"><SquareActivity size={14} /> Demo data</span>}
            <div className="node-status"><span className="pulse" /><div><strong>Origin node</strong><small>{snapshot.health === "online" ? "Online" : "Connecting…"}</small></div></div>
            <IconButton label="Refresh data" onClick={() => refresh()}><RefreshCw className={loading ? "spin" : ""} size={17} /></IconButton>
            <button className="primary-button desktop-create" onClick={() => setCreateType("stream")}><Plus size={17} /> New stream</button>
          </div>
        </header>
        <div className="content">
          {page === "overview" && <Overview snapshot={snapshot} history={history} bandwidth={bandwidth} onNavigate={setPage} onStreamAction={streamAction} />}
          {page === "streams" && <StreamsPage streams={sortedStreams} onAction={streamAction} openCreate={() => setCreateType("stream")} />}
          {page === "applications" && <ApplicationsPage applications={snapshot.applications} streams={snapshot.streams} openCreate={() => setCreateType("application")} />}
          {page === "capacity" && (
            <CapacityPage
              bandwidth={bandwidth}
              setBandwidth={setBandwidth}
              currentEgress={currentEgress}
              viewers={viewers}
              measuredBitrateMbps={measuredBitrateMbps}
              measuredBitrateSource={measuredBitrateSource}
            />
          )}
          {page === "system" && <SystemPage snapshot={snapshot} />}
        </div>
      </main>

      {notice && <div className={`toast ${notice.type}`}>{notice.type === "success" ? <Check size={18} /> : <X size={18} />}<span>{notice.message}</span></div>}

      {createType === "application" && client && (
        <CreateApplicationModal
          onClose={() => setCreateType(null)}
          onSubmit={(name) => perform(async () => { await client.createApplication(name); setCreateType(null); }, "Application created.")}
        />
      )}
      {createType === "stream" && client && (
        <CreateStreamModal
          applications={snapshot.applications}
          onClose={() => setCreateType(null)}
          onSubmit={(application, name, recording) =>
            perform(async () => {
              const data = await client.createStream(application, name, recording);
              setCreateType(null);
              setSecret({ title: "Stream is ready", data });
            }, "Stream created securely.")
          }
        />
      )}
      {secret && (
        <Modal title={secret.title} description="Copy this sensitive value now. The server will not reveal it again." onClose={() => setSecret(null)} wide>
          <div className="secret-warning"><LockKeyhole size={18} /><span>One-time secret · store it in your password manager</span></div>
          <div className="secret-fields">
            <CopyField label={secret.tokenExpires ? "Playback token" : "Publish key"} value={secret.data.stream_key} secret />
            {secret.data.publish_url && <CopyField label="OBS server URL" value={secret.data.publish_url} />}
            {secret.data.playback_url && <CopyField label="Playback URL" value={secret.data.playback_url} />}
          </div>
          {secret.tokenExpires && <p className="expiry-note">Expires {new Date(secret.tokenExpires * 1000).toLocaleString()}.</p>}
          <div className="modal-actions"><button className="primary-button" onClick={() => setSecret(null)}>I have stored it safely</button></div>
        </Modal>
      )}
      {actionStream && client && (
        <Modal title={actionStream.name} description={`${actionStream.application} / ${actionStream.name}`} onClose={() => setActionStream(null)}>
          <div className="action-menu">
            <button onClick={() => { setActionStream(null); streamAction(actionStream, "token"); }}><span><KeyRound size={18} /></span><div><strong>Issue playback token</strong><small>Generate a signed URL credential valid for one hour</small></div><ChevronRight size={17} /></button>
            <button onClick={() => perform(async () => { const data = await client.rotateKey(actionStream); setActionStream(null); setSecret({ title: "Publish key rotated", data }); }, "Old publish key revoked.")}><span><RefreshCw size={18} /></span><div><strong>Rotate publish key</strong><small>Immediately invalidate the previous publishing secret</small></div><ChevronRight size={17} /></button>
            <button onClick={() => perform(() => client.patchStream(actionStream, { recording_enabled: !actionStream.recording_enabled }), `Recording policy ${actionStream.recording_enabled ? "disabled" : "requested"}.`)}><span><FileVideo size={18} /></span><div><strong>{actionStream.recording_enabled ? "Disable" : "Request"} recording policy</strong><small>Persist the desired policy; runtime recording must also be enabled on the server</small></div><ChevronRight size={17} /></button>
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

function CreateStreamModal({
  applications,
  onClose,
  onSubmit
}: {
  applications: Application[];
  onClose: () => void;
  onSubmit: (application: string, name: string, recording: boolean) => void;
}) {
  const [application, setApplication] = useState(applications.find((app) => app.enabled)?.name ?? "");
  const [name, setName] = useState("");
  const [recording, setRecording] = useState(false);
  return (
    <Modal title="Create a secure stream" description="A one-time publish key will be generated after creation." onClose={onClose}>
      <form className="modal-form" onSubmit={(event: FormEvent) => { event.preventDefault(); onSubmit(application, name.trim(), recording); }}>
        <div className="two-fields">
          <label htmlFor="stream-application">Application<select id="stream-application" required value={application} onChange={(event) => setApplication(event.target.value)}><option value="">Select…</option>{applications.filter((app) => app.enabled).map((app) => <option key={app.name}>{app.name}</option>)}</select></label>
          <label htmlFor="stream-name">Public stream name<input id="stream-name" autoFocus required pattern="[A-Za-z0-9_-]+" value={name} onChange={(event) => setName(event.target.value)} placeholder="main-stage" /></label>
        </div>
        <label className="toggle-row"><span><FileVideo size={18} /><span><strong>Request recording policy</strong><small>Stores the per-stream policy; the direct installer keeps the runtime recorder disabled.</small></span></span><input type="checkbox" checked={recording} onChange={(event) => setRecording(event.target.checked)} /></label>
        {!applications.length && <div className="form-error">Create an application before creating a stream.</div>}
        <div className="modal-actions"><button type="button" className="secondary-button" onClick={onClose}>Cancel</button><button className="primary-button" disabled={!application || !name.trim()}><Plus size={17} /> Create stream</button></div>
      </form>
    </Modal>
  );
}

export default App;
