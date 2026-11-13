"use client";

import { Badge } from "@/components/catalyst/badge";
import { Button } from "@/components/catalyst/button";
import { Empty, Mono } from "@/components/ui";
import { fmtDateTime, shortHex } from "@/lib/format";
import type { OriginCert, PkiOrigin } from "@/lib/types";
import { ArrowDownTrayIcon } from "@heroicons/react/16/solid";
import { AnimatePresence, motion, MotionConfig } from "motion/react";
import type { ReactNode } from "react";

/** Offer `text` as a file download. */
function download(name: string, text: string) {
  const url = URL.createObjectURL(new Blob([text], { type: "application/x-pem-file" }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function labelOf(cert: OriginCert) {
  return cert.device ?? cert.subject_cn ?? cert.name ?? cert.subject ?? "certificado";
}

const row = {
  hidden: { opacity: 0, y: 6 },
  show: { opacity: 1, y: 0, transition: { duration: 0.22 } },
};

function Field({ term, children }: { term: string; children: ReactNode }) {
  return (
    <motion.div variants={row} className="grid grid-cols-[7.5rem_1fr] gap-x-3 text-sm">
      <span className="text-zinc-500">{term}</span>
      <span className="min-w-0 break-all text-zinc-950">{children}</span>
    </motion.div>
  );
}

/** One certificate. The fields arrive one after another; a new certificate replaces the card. */
function CertCard({ cert, role, delay = 0 }: { cert: OriginCert; role: string; delay?: number }) {
  const label = labelOf(cert);
  return (
    <motion.article
      layout
      data-testid={`origin-row-${cert.fingerprint}`}
      data-entity={role}
      initial={{ opacity: 0, y: 10 }}
      animate={{ opacity: 1, y: 0 }}
      exit={{ opacity: 0, y: -8 }}
      transition={{ duration: 0.24, delay }}
      className="rounded-lg bg-white p-4 ring-1 ring-zinc-950/10"
    >
      <div className="flex items-start justify-between gap-3">
        <div className="min-w-0">
          <p className="text-xs font-medium text-zinc-500">{role}</p>
          <h3 className="mt-1 truncate text-sm font-semibold text-zinc-950">{label}</h3>
        </div>
        {cert.current && <Badge color="sky">vigente</Badge>}
      </div>
      <motion.div
        initial="hidden"
        animate="show"
        variants={{ hidden: {}, show: { transition: { staggerChildren: 0.045, delayChildren: delay } } }}
        className="mt-3 space-y-1.5"
      >
        <Field term="Sujeto">{cert.subject_cn ?? cert.subject ?? label}</Field>
        <Field term="Serie">{cert.serial ? <Mono>{shortHex(cert.serial, 12)}</Mono> : "—"}</Field>
        <Field term="Huella">
          <span title={cert.fingerprint}>
            <Mono>{shortHex(cert.fingerprint, 16)}</Mono>
          </span>
        </Field>
        <Field term="Validez">
          <span className="tabular-nums">
            {fmtDateTime(cert.not_before)} → {fmtDateTime(cert.not_after)}
          </span>
        </Field>
      </motion.div>
      <div className="mt-3">
        <Button
          plain
          onClick={() => download(`${label}.pem`, cert.cert_pem.endsWith("\n") ? cert.cert_pem : `${cert.cert_pem}\n`)}
        >
          <ArrowDownTrayIcon />
          PEM
        </Button>
      </div>
    </motion.article>
  );
}

function Entity({
  role,
  cert,
  empty,
  testId,
}: {
  role: string;
  cert: OriginCert | null;
  empty: string;
  testId: string;
}) {
  return (
    <div data-testid={testId}>
      <AnimatePresence mode="wait">
        {cert ? (
          <CertCard key={cert.fingerprint} cert={cert} role={role} />
        ) : (
          <motion.div key="empty" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
            <Empty>{empty}</Empty>
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  );
}

/** Factory, manager and each worker, with the public fields of its certificate. */
export function OriginCircle({ origin }: { origin: PkiOrigin }) {
  return (
    <MotionConfig reducedMotion="user">
      <div className="grid grid-cols-1 gap-3 lg:grid-cols-2">
        <Entity role="Fábrica" testId="origin-factory" cert={origin.factory} empty="No hay raíz de fábrica registrada." />
        <Entity
          role="Manager"
          testId="origin-manager"
          cert={origin.manager}
          empty="El manager aún no ha registrado su certificado."
        />
      </div>
      <div className="mt-6" data-testid="origin-workers">
        <h3 className="mb-2 text-sm font-medium text-zinc-950">Workers</h3>
        {origin.workers.length === 0 ? (
          <Empty>Ningún worker tiene todavía un certificado firmado por la fábrica.</Empty>
        ) : (
          <motion.div layout className="grid grid-cols-1 gap-3 lg:grid-cols-2">
            <AnimatePresence>
              {origin.workers.map((cert, index) => (
                <CertCard key={cert.fingerprint} cert={cert} role="Worker" delay={Math.min(index, 8) * 0.04} />
              ))}
            </AnimatePresence>
          </motion.div>
        )}
      </div>
    </MotionConfig>
  );
}
