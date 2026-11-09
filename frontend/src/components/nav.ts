import {
  AdjustmentsHorizontalIcon,
  BoltIcon,
  CpuChipIcon,
  FingerPrintIcon,
  MapIcon,
  ServerStackIcon,
  ShieldCheckIcon,
  UsersIcon,
} from "@heroicons/react/20/solid";
import type { ComponentType, SVGProps } from "react";

export interface Tab {
  href: string;
  label: string;
  testId: string;
  icon: ComponentType<SVGProps<SVGSVGElement>>;
  /** Other path prefixes that belong to the tab, e.g. the device detail. */
  also?: string[];
}

/** Visualization panel: every role, read-only. */
export const VIEW_TABS: Tab[] = [
  { href: "/devices/", label: "Dispositivos", testId: "nav-devices", icon: CpuChipIcon, also: ["/device/"] },
  { href: "/config/", label: "Configuración", testId: "nav-config", icon: AdjustmentsHorizontalIcon },
  { href: "/map/", label: "Mapa", testId: "nav-map", icon: MapIcon },
  { href: "/pki/", label: "PKI", testId: "nav-pki", icon: ShieldCheckIcon },
  { href: "/events/", label: "Eventos", testId: "nav-events", icon: BoltIcon },
  { href: "/system/", label: "Sistema", testId: "nav-system", icon: ServerStackIcon },
];

/** Administration panel: admin only. */
export const ADMIN_TABS: Tab[] = [
  { href: "/admin/devices/", label: "Dispositivos", testId: "admin-tab-devices", icon: CpuChipIcon },
  { href: "/admin/identity/", label: "Identidad", testId: "admin-tab-identity", icon: FingerPrintIcon },
  { href: "/admin/config/", label: "Configuración", testId: "admin-tab-config", icon: AdjustmentsHorizontalIcon },
  { href: "/admin/users/", label: "Usuarios", testId: "admin-tab-users", icon: UsersIcon },
];

/** usePathname() with a trailing slash, as the static export serves pages. */
export function normalizePath(p: string | null): string {
  if (!p) return "/";
  return p.endsWith("/") ? p : `${p}/`;
}

export function isCurrent(tab: Tab, path: string): boolean {
  return [tab.href, ...(tab.also ?? [])].some((prefix) => path.startsWith(prefix));
}
