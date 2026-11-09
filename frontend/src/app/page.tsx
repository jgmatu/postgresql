"use client";

import { AuthLayout } from "@/components/catalyst/auth-layout";
import { Button } from "@/components/catalyst/button";
import { Field, Label } from "@/components/catalyst/fieldset";
import { Heading } from "@/components/catalyst/heading";
import { Input } from "@/components/catalyst/input";
import { Text } from "@/components/catalyst/text";
import { Callout } from "@/components/ui";
import { ApiError, describeError, login } from "@/lib/api";
import { useSession } from "@/lib/hooks";
import { useRouter, useSearchParams } from "next/navigation";
import { Suspense, useEffect, useState, type FormEvent } from "react";

function LoginForm() {
  const router = useRouter();
  const session = useSession();
  const expired = useSearchParams().get("expired") === "1";
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    if (session) router.replace("/devices/");
  }, [session, router]);

  async function submit(e: FormEvent<HTMLFormElement>) {
    e.preventDefault();
    const form = new FormData(e.currentTarget);
    setBusy(true);
    setError(null);
    try {
      await login(String(form.get("username") ?? ""), String(form.get("password") ?? ""));
      router.replace("/devices/");
    } catch (err) {
      setError(err instanceof ApiError && err.status === 401 ? "Usuario o contraseña incorrectos" : describeError(err));
    } finally {
      setBusy(false);
    }
  }

  return (
    <form onSubmit={submit} className="grid w-full max-w-sm grid-cols-1 gap-8" data-testid="login-form">
      <div>
        <Heading>Panel de control pgdemo</Heading>
        <Text className="mt-2">Dispositivos, identidad y telemetría sobre PostgreSQL. Entra con tu usuario.</Text>
      </div>
      {expired && !error && (
        <Callout tone="warning" testId="login-expired">
          La sesión ha caducado o el servidor ya no acepta el token. Vuelve a entrar.
        </Callout>
      )}
      {error && <Callout tone="error" testId="login-error">{error}</Callout>}
      <Field>
        <Label>Usuario</Label>
        <Input name="username" autoComplete="username" required autoFocus data-testid="login-username" />
      </Field>
      <Field>
        <Label>Contraseña</Label>
        <Input type="password" name="password" autoComplete="current-password" required data-testid="login-password" />
      </Field>
      <Button type="submit" className="w-full" disabled={busy} data-testid="login-submit">
        {busy ? "Entrando…" : "Entrar"}
      </Button>
    </form>
  );
}

export default function LoginPage() {
  return (
    <AuthLayout>
      <Suspense fallback={null}>
        <LoginForm />
      </Suspense>
    </AuthLayout>
  );
}
