"use client";

import { Avatar } from "@/components/catalyst/avatar";
import { Badge } from "@/components/catalyst/badge";
import {
  Dropdown,
  DropdownButton,
  DropdownDivider,
  DropdownHeader,
  DropdownItem,
  DropdownLabel,
  DropdownMenu,
} from "@/components/catalyst/dropdown";
import { Link } from "@/components/catalyst/link";
import { Navbar, NavbarDivider, NavbarItem, NavbarLabel, NavbarSection, NavbarSpacer } from "@/components/catalyst/navbar";
import {
  Sidebar,
  SidebarBody,
  SidebarDivider,
  SidebarHeader,
  SidebarHeading,
  SidebarItem,
  SidebarLabel,
  SidebarSection,
} from "@/components/catalyst/sidebar";
import { StackedLayout } from "@/components/catalyst/stacked-layout";
import { RoleBadge } from "@/components/badges";
import { ADMIN_TABS, isCurrent, normalizePath, VIEW_TABS } from "@/components/nav";
import { confirmIdentity } from "@/lib/api";
import { useHydrated, useSession } from "@/lib/hooks";
import { endSession, lastEndReason, type Session } from "@/lib/session";
import { ArrowRightStartOnRectangleIcon, ChevronDownIcon, WrenchScrewdriverIcon } from "@heroicons/react/20/solid";
import { usePathname, useRouter } from "next/navigation";
import { useEffect, type ReactNode } from "react";

function Brand() {
  return (
    <Link href="/devices/" className="flex items-center gap-2 px-2 font-semibold text-zinc-950" data-testid="brand">
      <svg viewBox="0 0 24 24" className="size-6" aria-hidden>
        <ellipse cx="12" cy="5" rx="8" ry="3" className="fill-sky-600" />
        <path d="M4 5v14c0 1.7 3.6 3 8 3s8-1.3 8-3V5c0 1.7-3.6 3-8 3S4 6.7 4 5Z" className="fill-sky-500" />
        <path d="M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3" className="fill-none stroke-white/70" strokeWidth="1.2" />
      </svg>
      <span>pgdemo</span>
    </Link>
  );
}

function UserMenu({ session }: { session: Session }) {
  const router = useRouter();
  const logout = () => {
    endSession("logout");
    router.replace("/");
  };
  return (
    <Dropdown>
      <DropdownButton as={NavbarItem} data-testid="user-menu" aria-label="Usuario">
        <Avatar initials={session.username.slice(0, 2)} className="bg-zinc-900 text-white" square />
        <span className="max-sm:hidden">{session.username}</span>
        <ChevronDownIcon />
      </DropdownButton>
      <DropdownMenu anchor="bottom end" className="min-w-64">
        <DropdownHeader>
          <div className="text-sm/6" data-testid="user-menu-header">
            <div className="flex items-center gap-2">
              <span className="font-semibold text-zinc-950" data-testid="user-name">
                {session.username}
              </span>
              <RoleBadge role={session.role} />
            </div>
            <div className="mt-1 text-xs text-zinc-500">
              PostgreSQL: <span className="font-mono" data-testid="user-pg-role">{session.pgRole ?? "…"}</span>
            </div>
          </div>
        </DropdownHeader>
        <DropdownDivider />
        <DropdownItem onClick={logout} data-testid="logout">
          <ArrowRightStartOnRectangleIcon />
          <DropdownLabel>Cerrar sesión</DropdownLabel>
        </DropdownItem>
      </DropdownMenu>
    </Dropdown>
  );
}

/**
 * Layout of every page after login: the visualization tabs for every role
 * and, for admins only, the way into the administration panel.
 *
 * Without a session it sends the browser back to the login page. The role in
 * the session is confirmed with /api/auth/me on load; the server enforces it
 * on every request anyway, so hiding the admin entry is presentation, not
 * security.
 */
export function PanelShell({ children }: { children: ReactNode }) {
  const session = useSession();
  const hydrated = useHydrated();
  const router = useRouter();
  const path = normalizePath(usePathname());

  useEffect(() => {
    if (hydrated && !session) router.replace(lastEndReason() === "expired" ? "/?expired=1" : "/");
  }, [hydrated, session, router]);

  const token = session?.token;
  useEffect(() => {
    if (!token) return;
    const ctrl = new AbortController();
    confirmIdentity(ctrl.signal).catch(() => {
      // a 401 has already ended the session; anything else leaves it as it was
    });
    return () => ctrl.abort();
  }, [token]);

  const expiresAt = session?.expiresAt;
  useEffect(() => {
    if (!expiresAt) return;
    const t = setTimeout(() => endSession("expired"), Math.max(0, expiresAt - Date.now()));
    return () => clearTimeout(t);
  }, [expiresAt]);

  if (!hydrated || !session) {
    return (
      <div className="p-10 text-sm text-zinc-500" data-testid="panel-loading">
        Cargando…
      </div>
    );
  }

  const isAdmin = session.role === "admin";
  const inAdmin = path.startsWith("/admin/");

  return (
    <StackedLayout
      navbar={
        <Navbar>
          <Brand />
          <NavbarDivider className="max-lg:hidden" />
          <NavbarSection className="max-lg:hidden" data-testid="nav-view">
            {VIEW_TABS.map((t) => (
              <NavbarItem key={t.href} href={t.href} current={!inAdmin && isCurrent(t, path)} data-testid={t.testId}>
                <t.icon />
                <NavbarLabel>{t.label}</NavbarLabel>
              </NavbarItem>
            ))}
            {isAdmin && (
              <>
                <NavbarDivider />
                <NavbarItem href="/admin/devices/" current={inAdmin} data-testid="nav-admin">
                  <WrenchScrewdriverIcon className="fill-amber-600!" />
                  <NavbarLabel>Administración</NavbarLabel>
                  <Badge color="amber">admin</Badge>
                </NavbarItem>
              </>
            )}
          </NavbarSection>
          <NavbarSpacer />
          <NavbarSection>
            <UserMenu session={session} />
          </NavbarSection>
        </Navbar>
      }
      sidebar={
        <Sidebar>
          <SidebarHeader>
            <Brand />
          </SidebarHeader>
          <SidebarBody>
            <SidebarSection>
              {VIEW_TABS.map((t) => (
                <SidebarItem key={t.href} href={t.href} current={!inAdmin && isCurrent(t, path)}>
                  <t.icon />
                  <SidebarLabel>{t.label}</SidebarLabel>
                </SidebarItem>
              ))}
            </SidebarSection>
            {isAdmin && (
              <>
                <SidebarDivider />
                <SidebarSection data-testid="sidebar-admin">
                  <SidebarHeading>
                    Administración <Badge color="amber">admin</Badge>
                  </SidebarHeading>
                  {ADMIN_TABS.map((t) => (
                    <SidebarItem key={t.href} href={t.href} current={path.startsWith(t.href)}>
                      <t.icon />
                      <SidebarLabel>{t.label}</SidebarLabel>
                    </SidebarItem>
                  ))}
                </SidebarSection>
              </>
            )}
          </SidebarBody>
        </Sidebar>
      }
    >
      {children}
    </StackedLayout>
  );
}
