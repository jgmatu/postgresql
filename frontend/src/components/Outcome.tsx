import { Callout } from "@/components/ui";
import type { ReactNode } from "react";

/** What an admin action ended in. `state` is for the UI tests (`data-state`). */
export interface Outcome {
  state: string;
  tone: "success" | "info" | "warning" | "error";
  title: ReactNode;
  detail?: ReactNode;
}

export function OutcomeCallout({ outcome, testId }: { outcome: Outcome | null; testId: string }) {
  if (!outcome) return null;
  return (
    <div data-testid={testId} data-state={outcome.state} className="mb-6">
      <Callout tone={outcome.tone} title={outcome.title}>
        {outcome.detail}
      </Callout>
    </div>
  );
}
