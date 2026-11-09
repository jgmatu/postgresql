"use client";

import { Badge } from "@/components/catalyst/badge";
import { Link } from "@/components/catalyst/link";
import { ADMIN_TABS, normalizePath } from "@/components/nav";
import { Loading } from "@/components/ui";
import { useSession } from "@/lib/hooks";
import clsx from "clsx";
import { usePathname, useRouter } from "next/navigation";
import { useEffect, type ReactNode } from "react";

/**
 * Administration panel: redirects anyone but an admin to the visualization,
 * and frames the pages in amber with their own sub-tabs, so it is always
 * clear that actions here change the fleet.
 */
export function AdminShell({ children }: { children: ReactNode }) {
  const session = useSession();
  const router = useRouter();
  const path = normalizePath(usePathname());
  const allowed = session?.role === "admin";

  useEffect(() => {
    if (session && !allowed) router.replace("/devices/");
  }, [session, allowed, router]);

  if (!allowed) return <Loading label="Comprobando permisos…" />;

  return (
    <div data-testid="admin-panel">
      <div className="-mx-2 mb-6 flex flex-wrap items-center gap-3 rounded-lg border-l-4 border-amber-500 bg-amber-50 px-4 py-3">
        <span className="font-semibold text-amber-900">Administración</span>
        <Badge color="amber">admin</Badge>
        <span className="text-sm text-amber-800">
          Estas acciones modifican la flota; el servidor exige el rol admin en cada petición.
        </span>
      </div>
      <nav className="mb-8 flex flex-wrap gap-1 border-b border-zinc-950/10" aria-label="Administración">
        {ADMIN_TABS.map((t) => {
          const current = path.startsWith(t.href);
          return (
            <Link
              key={t.href}
              href={t.href}
              data-testid={t.testId}
              aria-current={current ? "page" : undefined}
              className={clsx(
                "-mb-px flex items-center gap-2 border-b-2 px-3 py-2 text-sm font-medium whitespace-nowrap",
                current ? "border-amber-500 text-amber-900" : "border-transparent text-zinc-600 hover:border-zinc-300 hover:text-zinc-900",
              )}
            >
              <t.icon className={clsx("size-4", current ? "fill-amber-600" : "fill-zinc-400")} />
              {t.label}
            </Link>
          );
        })}
      </nav>
      {children}
    </div>
  );
}
