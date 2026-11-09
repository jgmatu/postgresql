import { PanelShell } from "@/components/PanelShell";
import type { ReactNode } from "react";

export default function PanelLayout({ children }: { children: ReactNode }) {
  return <PanelShell>{children}</PanelShell>;
}
