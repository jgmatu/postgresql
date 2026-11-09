"use client";

import { Button } from "@/components/catalyst/button";
import { Dialog, DialogActions, DialogBody, DialogDescription, DialogTitle } from "@/components/catalyst/dialog";
import type { ReactNode } from "react";

/** Confirmation for a destructive admin action. */
export function ConfirmDialog({
  open,
  title,
  children,
  confirmLabel,
  onConfirm,
  onClose,
  busy = false,
  testId,
}: {
  open: boolean;
  title: ReactNode;
  children: ReactNode;
  confirmLabel: string;
  onConfirm: () => void;
  onClose: () => void;
  busy?: boolean;
  testId: string;
}) {
  return (
    <Dialog open={open} onClose={onClose} size="md">
      <div data-testid={testId}>
        <DialogTitle>{title}</DialogTitle>
        <DialogDescription>Esta acción no se puede deshacer desde el panel.</DialogDescription>
        <DialogBody className="text-sm/6 text-zinc-700">{children}</DialogBody>
        <DialogActions>
          <Button plain onClick={onClose} data-testid={`${testId}-cancel`}>
            Cancelar
          </Button>
          <Button color="red" onClick={onConfirm} disabled={busy} data-testid={`${testId}-confirm`}>
            {confirmLabel}
          </Button>
        </DialogActions>
      </div>
    </Dialog>
  );
}
