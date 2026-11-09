"use client";

import { Button } from "@/components/catalyst/button";
import { Description, Field, FieldGroup, Fieldset, Label, Legend } from "@/components/catalyst/fieldset";
import { Input } from "@/components/catalyst/input";
import { Select } from "@/components/catalyst/select";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/catalyst/table";
import { RoleBadge } from "@/components/badges";
import { ConfirmDialog } from "@/components/ConfirmDialog";
import { OutcomeCallout, type Outcome } from "@/components/Outcome";
import { ErrorCallout, Loading, PageHeading } from "@/components/ui";
import { ApiError, api, describeError } from "@/lib/api";
import { fmtDateTime } from "@/lib/format";
import { useApi, useSession } from "@/lib/hooks";
import type { Role, User } from "@/lib/types";
import { TrashIcon, UserPlusIcon } from "@heroicons/react/16/solid";
import { useState, type FormEvent } from "react";

export default function AdminUsersPage() {
  const session = useSession();
  const users = useApi<User[]>("/api/users");
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [role, setRole] = useState<Role>("normal");
  const [busy, setBusy] = useState(false);
  const [deleting, setDeleting] = useState<string | null>(null);
  const [outcome, setOutcome] = useState<Outcome | null>(null);

  async function create(e: FormEvent) {
    e.preventDefault();
    setBusy(true);
    try {
      await api("/api/users", { method: "POST", body: { username, password, role } });
      setOutcome({ state: "created", tone: "success", title: `Usuario ${username} creado (${role})` });
      setUsername("");
      setPassword("");
    } catch (err) {
      const conflict = err instanceof ApiError && err.status === 409;
      setOutcome({ state: conflict ? "conflict" : "error", tone: "error", title: conflict ? `«${username}» ya existe` : "No se pudo crear", detail: describeError(err) });
    } finally {
      setBusy(false);
      users.reload();
    }
  }

  async function remove(name: string) {
    setBusy(true);
    try {
      await api(`/api/users/${encodeURIComponent(name)}`, { method: "DELETE" });
      setOutcome({ state: "deleted", tone: "success", title: `Usuario ${name} eliminado`, detail: "Sus tokens dejan de valer al caducar; el servidor no guarda sesiones." });
    } catch (err) {
      const last = err instanceof ApiError && err.status === 422;
      setOutcome({
        state: last ? "last-admin" : "error",
        tone: "error",
        title: last ? "No se puede eliminar el último admin" : `No se pudo eliminar ${name}`,
        detail: last ? "PostgreSQL lo impide con un trigger: siempre queda al menos un administrador." : describeError(err),
      });
    } finally {
      setBusy(false);
      setDeleting(null);
      users.reload();
    }
  }

  return (
    <>
      <PageHeading
        title="Usuarios"
        description="Directorio app_users, solo accesible con el rol admin (y con SET LOCAL ROLE demo_admin en PostgreSQL)."
        testId="page-admin-users"
      />
      <div className="mt-6 grid gap-10 lg:grid-cols-[minmax(0,1fr)_20rem]">
        <div className="min-w-0">
          <OutcomeCallout outcome={outcome} testId="users-result" />
          <ErrorCallout error={users.error} />
          {users.loading && !users.data ? (
            <Loading />
          ) : (
            <Table data-testid="users-table">
              <TableHead>
                <TableRow>
                  <TableHeader>Usuario</TableHeader>
                  <TableHeader>Rol</TableHeader>
                  <TableHeader>Creado</TableHeader>
                  <TableHeader className="text-right">Acciones</TableHeader>
                </TableRow>
              </TableHead>
              <TableBody>
                {(users.data ?? []).map((u) => (
                  <TableRow key={u.user_id} data-testid={`user-row-${u.username}`}>
                    <TableCell className="font-medium">
                      {u.username}
                      {u.username === session?.username && <span className="ml-2 text-xs text-zinc-500">(tú)</span>}
                    </TableCell>
                    <TableCell>
                      <RoleBadge role={u.role} testId={`user-role-${u.username}`} />
                    </TableCell>
                    <TableCell className="tabular-nums">{fmtDateTime(u.created_at)}</TableCell>
                    <TableCell className="text-right">
                      <Button plain disabled={busy} onClick={() => setDeleting(u.username)} data-testid={`user-delete-${u.username}`}>
                        <TrashIcon className="fill-red-500!" />
                        Eliminar
                      </Button>
                    </TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          )}
        </div>
        <form onSubmit={create} className="rounded-lg p-5 ring-1 ring-zinc-950/10" data-testid="user-form">
          <Fieldset>
            <Legend>Nuevo usuario</Legend>
            <FieldGroup>
              <Field>
                <Label>Usuario</Label>
                <Input value={username} onChange={(e) => setUsername(e.target.value)} required maxLength={64} autoComplete="off" data-testid="user-username" />
              </Field>
              <Field>
                <Label>Contraseña</Label>
                <Input type="password" value={password} onChange={(e) => setPassword(e.target.value)} required minLength={12} autoComplete="new-password" data-testid="user-password" />
                <Description>Al menos 12 caracteres; se guarda como hash Argon2id.</Description>
              </Field>
              <Field>
                <Label>Rol</Label>
                <Select value={role} onChange={(e) => setRole(e.target.value as Role)} data-testid="user-role">
                  <option value="normal">normal — solo lectura</option>
                  <option value="admin">admin — lectura y escritura</option>
                </Select>
              </Field>
            </FieldGroup>
          </Fieldset>
          <Button type="submit" color="amber" className="mt-6 w-full" disabled={busy} data-testid="user-create">
            <UserPlusIcon />
            Crear usuario
          </Button>
        </form>
      </div>
      <ConfirmDialog
        open={deleting !== null}
        title={`Eliminar ${deleting ?? ""}`}
        confirmLabel="Eliminar usuario"
        busy={busy}
        onClose={() => setDeleting(null)}
        onConfirm={() => deleting && remove(deleting)}
        testId="dialog-delete-user"
      >
        {deleting === session?.username ? "Es tu propio usuario: tu sesión seguirá viva hasta que caduque el token." : "El usuario deja de poder entrar."}
      </ConfirmDialog>
    </>
  );
}
