import { useCallback, useEffect, useMemo, useState } from "react";
import { useTheme } from "next-themes";
import { Moon, Sun } from "lucide-react";
import {
  CartesianGrid,
  Cell,
  Legend,
  Line,
  LineChart,
  Pie,
  PieChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";
import { Alert, AlertDescription, AlertTitle } from "@/components/ui/alert";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Progress } from "@/components/ui/progress";
import { Separator } from "@/components/ui/separator";
import { Skeleton } from "@/components/ui/skeleton";

const RPS_HISTORY = 5;

type Metrics = {
  active_connections: number;
  total_requests: number;
  requests_per_second: number;
  cache_hit_rate: number;
  cache_hits: number;
  cache_misses: number;
  approximate_memory_usage_bytes: number;
  cache_usage_bytes: number;
  cache_usage_ratio: number;
  thread_count: number;
  cache_alpha: number;
  cache_beta: number;
};

const PIE_COLORS = ["#22c55e", "#f87171"];

export function MonitorDashboard() {
  const { theme, setTheme } = useTheme();
  const [metrics, setMetrics] = useState<Metrics | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [rpsSeries, setRpsSeries] = useState<
    { t: string; rps: number }[]
  >([]);
  const [updatedAt, setUpdatedAt] = useState<string | null>(null);

  const effectiveRps = metrics ? metrics.requests_per_second : null;

  const fetchMetrics = useCallback(async () => {
    try {
      const res = await fetch("/metrics");
      if (!res.ok) {
        throw new Error(`HTTP ${res.status}`);
      }
      const m = (await res.json()) as Metrics;
      setMetrics(m);
      setError(null);
      setLoading(false);
      setUpdatedAt(new Date().toLocaleTimeString());
      const label = new Date().toLocaleTimeString();
      setRpsSeries((prev) => {
        const next = [...prev, { t: label, rps: m.requests_per_second }];
        return next.length > RPS_HISTORY ? next.slice(-RPS_HISTORY) : next;
      });
    } catch (e) {
      const msg = e instanceof Error ? e.message : "Unknown error";
      setError(`Could not load metrics. ${msg}`);
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    queueMicrotask(() => {
      void fetchMetrics();
    });
    const id = window.setInterval(() => {
      if (!document.hidden) {
        void fetchMetrics();
      }
    }, 1000);
    return () => window.clearInterval(id);
  }, [fetchMetrics]);

  useEffect(() => {
    const onVis = () => {
      if (!document.hidden) {
        void fetchMetrics();
      }
    };
    document.addEventListener("visibilitychange", onVis);
    return () => document.removeEventListener("visibilitychange", onVis);
  }, [fetchMetrics]);

  const strategy = useMemo(() => {
    if (!metrics) {
      return "";
    }
    if (metrics.cache_hit_rate < 0.5) {
      return "Recency-heavy";
    }
    if (metrics.cache_hit_rate > 0.8) {
      return "Frequency-heavy";
    }
    return "Balanced";
  }, [metrics]);

  const pieData = useMemo(() => {
    if (!metrics) {
      return [];
    }
    const totalCacheEvents = metrics.cache_hits + metrics.cache_misses;
    if (totalCacheEvents === 0) {
      return [];
    }
    return [
      { name: "Hits", value: metrics.cache_hits },
      { name: "Misses", value: metrics.cache_misses },
    ];
  }, [metrics]);

  const toggleTheme = () => {
    setTheme(theme === "dark" ? "light" : "dark");
  };

  return (
    <div className="min-h-svh bg-background text-foreground">
      <Button
        type="button"
        variant="outline"
        size="icon"
        className="fixed top-4 right-4 z-20 h-9 w-9 rounded-full border-border/80 bg-card/85 shadow-sm backdrop-blur-sm"
        onClick={toggleTheme}
        aria-label={theme === "dark" ? "Use light theme" : "Use dark theme"}
        title={theme === "dark" ? "Switch to light mode" : "Switch to dark mode"}
      >
        {theme === "dark" ? (
          <Sun className="size-4" aria-hidden />
        ) : (
          <Moon className="size-4" aria-hidden />
        )}
      </Button>

      <header className="border-b bg-card/30">
        <div className="mx-auto flex max-w-6xl flex-col gap-3 px-4 py-5 pr-16 sm:flex-row sm:items-center sm:justify-between md:px-8 md:pr-20">
          <div>
            <h1 className="text-xl font-semibold tracking-tight md:text-2xl">
              Concurrent HTTP Server
            </h1>
            {updatedAt ? (
              <p className="text-muted-foreground text-sm">
                Updated at {updatedAt}
              </p>
            ) : null}
          </div>
          <Badge variant="outline" className="w-fit">
            Live metrics
          </Badge>
        </div>
      </header>

      <main className="mx-auto max-w-6xl space-y-8 px-4 py-8 md:px-8">
        {error ? (
          <Alert variant="destructive">
            <AlertTitle>Error</AlertTitle>
            <AlertDescription>{error}</AlertDescription>
          </Alert>
        ) : null}

        <section className="grid gap-4 sm:grid-cols-2 lg:grid-cols-4">
          {loading && !metrics ? (
            <>
              {Array.from({ length: 4 }).map((_, i) => (
                <Card key={i}>
                  <CardHeader className="pb-2">
                    <Skeleton className="h-3 w-24" />
                  </CardHeader>
                  <CardContent>
                    <Skeleton className="h-8 w-16" />
                  </CardContent>
                </Card>
              ))}
            </>
          ) : metrics ? (
            <>
              <MetricCard
                label="Connections"
                value={String(metrics.active_connections)}
              />
              <MetricCard
                label="Req/sec"
                value={(effectiveRps ?? metrics.requests_per_second).toFixed(2)}
              />
              <MetricCard
                label="Cache hit"
                value={`${(metrics.cache_hit_rate * 100).toFixed(1)}%`}
              />
              <MetricCard
                label="Threads"
                value={String(metrics.thread_count)}
              />
            </>
          ) : null}
        </section>

        {metrics ? (
          <>
            <Progress
              className="h-2"
              value={Math.min(100, metrics.cache_usage_ratio * 100)}
            />
            <p className="text-muted-foreground text-xs">
              Cache usage{" "}
              <span className="text-foreground font-medium tabular-nums">
                {(metrics.cache_usage_ratio * 100).toFixed(2)}%
              </span>
            </p>
          </>
        ) : null}

        <Separator />

        <section className="space-y-4">
          <div>
            <h2 className="text-lg font-semibold tracking-tight">Performance</h2>
            <p className="text-muted-foreground text-sm">
              Cache mix and request rate over time
            </p>
          </div>
          <div className="grid gap-6 lg:grid-cols-2">
            <Card>
              <CardHeader>
                <CardTitle className="text-base">Cache performance</CardTitle>
                <CardDescription>Hits vs misses</CardDescription>
              </CardHeader>
              <CardContent className="h-72">
                {pieData.length > 0 ? (
                  <ResponsiveContainer width="100%" height="100%">
                    <PieChart>
                      <Pie
                        data={pieData}
                        dataKey="value"
                        nameKey="name"
                        cx="50%"
                        cy="50%"
                        innerRadius={56}
                        outerRadius={88}
                        paddingAngle={2}
                      >
                        {pieData.map((_, i) => (
                          <Cell
                            key={i}
                            fill={PIE_COLORS[i % PIE_COLORS.length]}
                          />
                        ))}
                      </Pie>
                      <Tooltip />
                      <Legend />
                    </PieChart>
                  </ResponsiveContainer>
                ) : metrics ? (
                  <div className="flex h-full items-center justify-center rounded-lg border border-dashed border-border text-center text-sm text-muted-foreground">
                    No cache hits or misses yet.
                  </div>
                ) : (
                  <Skeleton className="h-full w-full rounded-lg" />
                )}
              </CardContent>
            </Card>

            <Card>
              <CardHeader>
                <CardTitle className="text-base">Request load</CardTitle>
                <CardDescription>1-second request rate over the last 5 samples</CardDescription>
              </CardHeader>
              <CardContent className="h-72">
                {rpsSeries.length > 0 ? (
                  <ResponsiveContainer width="100%" height="100%">
                    <LineChart data={rpsSeries}>
                      <CartesianGrid strokeDasharray="3 3" stroke="var(--border)" />
                      <XAxis dataKey="t" tick={{ fontSize: 11 }} />
                      <YAxis tick={{ fontSize: 11 }} width={40} />
                      <Tooltip />
                      <Legend />
                      <Line
                        type="linear"
                        dataKey="rps"
                        name="Req/sec"
                        stroke="#3b82f6"
                        strokeWidth={2}
                        dot={false}
                      />
                    </LineChart>
                  </ResponsiveContainer>
                ) : (
                  <Skeleton className="h-full w-full rounded-lg" />
                )}
              </CardContent>
            </Card>
          </div>
        </section>

        <Separator />

        <section className="space-y-4">
          <h2 className="text-lg font-semibold tracking-tight">Summary</h2>
          <div className="grid gap-4 sm:grid-cols-3">
            {metrics ? (
              <>
                <MetricCard
                  label="Total requests"
                  value={String(metrics.total_requests)}
                />
                <MetricCard
                  label="Cache memory"
                  value={`${(metrics.cache_usage_bytes / 1024).toFixed(2)} KB`}
                />
                <MetricCard
                  label="Cache usage"
                  value={`${(Math.floor(metrics.cache_usage_ratio * 100000) / 1000).toFixed(3)}%`}
                />
              </>
            ) : (
              <>
                <Skeleton className="h-28 rounded-xl" />
                <Skeleton className="h-28 rounded-xl" />
                <Skeleton className="h-28 rounded-xl" />
              </>
            )}
          </div>
        </section>

        {metrics ? (
          <div className="grid gap-4 md:grid-cols-2">
            <Card>
              <CardHeader className="pb-2">
                <CardTitle className="text-base">Cache strategy</CardTitle>
              </CardHeader>
              <CardContent className="flex items-center gap-2">
                <Badge variant="secondary">{strategy}</Badge>
              </CardContent>
            </Card>
            <Card>
              <CardHeader className="pb-2">
                <CardTitle className="text-base">Alpha / Beta</CardTitle>
              </CardHeader>
              <CardContent className="text-lg font-semibold tabular-nums">
                {metrics.cache_alpha.toFixed(1)} / {metrics.cache_beta.toFixed(1)}
              </CardContent>
            </Card>
          </div>
        ) : null}
      </main>
    </div>
  );
}

function MetricCard({ label, value }: { label: string; value: string }) {
  return (
    <Card>
      <CardHeader className="pb-2">
        <CardDescription className="text-xs font-medium tracking-wide uppercase">
          {label}
        </CardDescription>
      </CardHeader>
      <CardContent>
        <p className="text-2xl font-semibold tabular-nums tracking-tight">
          {value}
        </p>
      </CardContent>
    </Card>
  );
}
