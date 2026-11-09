import { Heading } from "@/components/catalyst/heading";
import { Text } from "@/components/catalyst/text";
import type { StreamState } from "@/lib/sse";
import { describeError } from "@/lib/api";
import { ExclamationTriangleIcon, CheckCircleIcon, InformationCircleIcon } from "@heroicons/react/20/solid";
import clsx from "clsx";
import type { ReactNode } from "react";

/** Page title with an optional description and actions on the right. */
export function PageHeading({
  title,
  description,
  actions,
  testId,
}: {
  title: ReactNode;
  description?: ReactNode;
  actions?: ReactNode;
  testId?: string;
}) {
  return (
    <div className="flex flex-wrap items-end justify-between gap-4 border-b border-zinc-950/10 pb-5" data-testid={testId}>
      <div className="min-w-0">
        <Heading>{title}</Heading>
        {description && <Text className="mt-1">{description}</Text>}
      </div>
      {actions && <div className="flex flex-wrap items-center gap-3">{actions}</div>}
    </div>
  );
}

const tones = {
  error: ["bg-red-50 text-red-900 ring-red-600/20", ExclamationTriangleIcon, "fill-red-500"],
  info: ["bg-sky-50 text-sky-900 ring-sky-600/20", InformationCircleIcon, "fill-sky-500"],
  success: ["bg-green-50 text-green-900 ring-green-600/20", CheckCircleIcon, "fill-green-500"],
  warning: ["bg-amber-50 text-amber-900 ring-amber-600/30", ExclamationTriangleIcon, "fill-amber-500"],
} as const;

/** Inline message box. Catalyst's Alert is a modal; this one sits in the page. */
export function Callout({
  tone = "info",
  title,
  children,
  testId,
}: {
  tone?: keyof typeof tones;
  title?: ReactNode;
  children?: ReactNode;
  testId?: string;
}) {
  const [cls, Icon, iconCls] = tones[tone];
  return (
    <div role={tone === "error" ? "alert" : "status"} data-testid={testId} className={clsx("flex gap-3 rounded-lg p-3 text-sm/6 ring-1", cls)}>
      <Icon className={clsx("mt-0.5 size-5 shrink-0", iconCls)} aria-hidden />
      <div className="min-w-0">
        {title && <p className="font-semibold">{title}</p>}
        {children && <div className="break-words">{children}</div>}
      </div>
    </div>
  );
}

/** An error from `api()` as a callout. */
export function ErrorCallout({ error, testId = "error" }: { error: unknown; testId?: string }) {
  if (!error) return null;
  return (
    <Callout tone="error" testId={testId}>
      {describeError(error)}
    </Callout>
  );
}

export function Loading({ label = "Cargando…" }: { label?: string }) {
  return (
    <p className="animate-pulse py-6 text-sm text-zinc-500" data-testid="loading">
      {label}
    </p>
  );
}

export function Empty({ children }: { children: ReactNode }) {
  return <p className="py-6 text-center text-sm text-zinc-500">{children}</p>;
}

/** A titled block inside a page. */
export function Section({
  title,
  actions,
  children,
  className,
  testId,
}: {
  title: ReactNode;
  actions?: ReactNode;
  children: ReactNode;
  className?: string;
  testId?: string;
}) {
  return (
    <section className={clsx("mt-10", className)} data-testid={testId}>
      <div className="mb-3 flex flex-wrap items-center justify-between gap-3">
        <h2 className="text-base/7 font-semibold text-zinc-950 sm:text-sm/6">{title}</h2>
        {actions}
      </div>
      {children}
    </section>
  );
}

/** Small stat card: label and value. */
export function Stat({ label, value, testId }: { label: ReactNode; value: ReactNode; testId?: string }) {
  return (
    <div className="rounded-lg bg-zinc-50 p-4 ring-1 ring-zinc-950/5" data-testid={testId}>
      <div className="text-sm text-zinc-500">{label}</div>
      <div className="mt-1 text-2xl/8 font-semibold tabular-nums text-zinc-950">{value}</div>
    </div>
  );
}

const streamLabels: Record<StreamState, [string, string]> = {
  connecting: ["Conectando", "bg-sky-500"],
  open: ["En vivo", "bg-green-500"],
  retrying: ["Reconectando", "bg-amber-500"],
  closed: ["Cerrado", "bg-zinc-400"],
};

/** State of the SSE stream: a dot and a word. */
export function LiveIndicator({ state, detail }: { state: StreamState; detail?: string }) {
  const [label, dot] = streamLabels[state];
  return (
    <span className="inline-flex items-center gap-2 text-sm text-zinc-600" data-testid="stream-state" data-state={state} title={detail}>
      <span className={clsx("size-2 rounded-full", dot, state === "open" && "animate-pulse")} />
      {label}
    </span>
  );
}

/** Monospace value that can be selected and copied whole. */
export function Mono({ children, className }: { children: ReactNode; className?: string }) {
  return <span className={clsx("font-mono text-xs break-all select-all", className)}>{children}</span>;
}
